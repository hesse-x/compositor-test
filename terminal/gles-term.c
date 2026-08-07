// gles-term —— GLES 渲染的真终端（Wayland 客户端）。
//   · forkpty() 起 $SHELL：键盘输入写进 PTY，shell 输出读回来解析
//   · 内置最小 VT 模拟器：UTF-8、CSI（光标/清屏/滚动/插入删除）、
//     SGR 16/256/真彩色、备用屏幕（less/htop 可用）、OSC 标题
//   · 文字用 freetype 光栅化成字形图集纹理，GLES2 直接绘制
//     （不依赖 cairo：矩形/圆角/圆/字符全在 GPU 上画）
//   · 标题栏和边框由 libtinyui 统一绘制，窗口动画由 compositor 负责
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pty.h>                   // forkpty
#include <linux/input-event-codes.h>  // BTN_LEFT

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "tinyui.h"

#define FONT_SIZE 16.0
#define CORNER_RADIUS 10.0
#define TERM_PAD 12.0

#define MAX_COLS 512
#define MAX_ROWS 512

// 颜色存 0xRRGGBB；COLOR_DEFAULT 表示"用默认色"（fg 用浅绿白，bg 不填充）
#define COLOR_DEFAULT UINT32_MAX

#define CELL_INVERSE 1

// ---------------------------------------------------------------------------
// 终端模拟器：单元格网格 + ANSI 转义序列解析
// ---------------------------------------------------------------------------
struct cell {
	uint32_t cp;     // Unicode 码点（0 = 空）
	uint32_t fg, bg; // 0xRRGGBB 或 COLOR_DEFAULT
	uint8_t flags;   // CELL_INVERSE
};

enum { P_GROUND, P_ESC, P_ESC_SKIP, P_CSI, P_OSC };

struct term {
	int cols, rows;
	struct cell *grid;      // 主屏幕
	struct cell *alt;       // 备用屏幕（ESC[?1049h，less/htop/vim 用）
	bool alt_active;
	int cx, cy;
	int saved_cx, saved_cy;
	int scroll_top, scroll_bottom;
	uint32_t fg, bg;
	bool bold, inverse;
	bool wrap_pending;      // 光标停在最右列，下一个字符先换行
	bool cursor_visible;

	// 解析器状态
	int pstate;
	int params[16];
	int nparams;
	bool priv;              // CSI 带 '?' 前缀
	char osc[256];
	int osc_len;
	uint32_t utf8_cp;       // 未收完的 UTF-8 序列
	int utf8_need;
};

// xterm 256 色调色板：0-15 基本色，16-231 是 6×6×6 立方体，232-255 灰阶
static uint32_t palette[256];

static void palette_init(void) {
	static const uint32_t base[16] = {
		0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
		0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
		0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
		0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
	};
	memcpy(palette, base, sizeof(base));
	static const int lvl[6] = { 0, 95, 135, 175, 215, 255 };
	for (int i = 0; i < 216; i++)
		palette[16 + i] = (lvl[i / 36] << 16) | (lvl[(i / 6) % 6] << 8) | lvl[i % 6];
	for (int i = 0; i < 24; i++) {
		uint32_t g = 8 + 10 * i;
		palette[232 + i] = (g << 16) | (g << 8) | g;
	}
}

static struct term term;

static struct cell *cur_grid(void) {
	return term.alt_active ? term.alt : term.grid;
}

static struct cell *cell_at(int x, int y) {
	return &cur_grid()[y * term.cols + x];
}

static struct cell blank_cell(void) {
	// 擦除用当前 SGR 背景色填充（xterm 行为，彩色屏保/进度条才正常）
	return (struct cell){ .cp = 0, .fg = COLOR_DEFAULT, .bg = term.bg, .flags = 0 };
}

static void clear_row(int y, int x0, int x1) {
	struct cell b = blank_cell();
	for (int x = x0; x <= x1; x++)
		*cell_at(x, y) = b;
}

static void clear_rows(int y0, int y1) {
	for (int y = y0; y <= y1; y++)
		clear_row(y, 0, term.cols - 1);
}

static void scroll_up(int top, int bottom, int n) {
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	struct cell *g = cur_grid();
	memmove(&g[top * term.cols], &g[(top + n) * term.cols],
			(size_t)(bottom - top + 1 - n) * term.cols * sizeof(struct cell));
	clear_rows(bottom - n + 1, bottom);
}

static void scroll_down(int top, int bottom, int n) {
	if (n > bottom - top + 1)
		n = bottom - top + 1;
	struct cell *g = cur_grid();
	memmove(&g[(top + n) * term.cols], &g[top * term.cols],
			(size_t)(bottom - top + 1 - n) * term.cols * sizeof(struct cell));
	clear_rows(top, top + n - 1);
}

static void newline(void) {
	term.wrap_pending = false;
	if (term.cy == term.scroll_bottom)
		scroll_up(term.scroll_top, term.scroll_bottom, 1);
	else if (term.cy < term.rows - 1)
		term.cy++;
}

static void put_char(uint32_t cp) {
	if (term.wrap_pending) {
		term.cx = 0;
		newline();
	}
	struct cell *c = cell_at(term.cx, term.cy);
	c->cp = cp;
	c->fg = term.fg;
	c->bg = term.bg;
	c->flags = term.inverse ? CELL_INVERSE : 0;
	if (term.cx == term.cols - 1)
		term.wrap_pending = true;
	else
		term.cx++;
}

// ---------------------------------------------------------------------------
// SGR：颜色/反色
// ---------------------------------------------------------------------------
static void term_sgr(void) {
	for (int i = 0; i < term.nparams; i++) {
		int p = term.params[i];
		if (p == 0) {
			term.fg = COLOR_DEFAULT;
			term.bg = COLOR_DEFAULT;
			term.bold = false;
			term.inverse = false;
		} else if (p == 1) {
			term.bold = true;
		} else if (p == 22) {
			term.bold = false;
		} else if (p == 7) {
			term.inverse = true;
		} else if (p == 27) {
			term.inverse = false;
		} else if (p == 39) {
			term.fg = COLOR_DEFAULT;
		} else if (p == 49) {
			term.bg = COLOR_DEFAULT;
		} else if (p >= 30 && p <= 37) {
			// bold 用亮色代替加粗字体（排版的简化）
			term.fg = palette[p - 30 + (term.bold ? 8 : 0)];
		} else if (p >= 40 && p <= 47) {
			term.bg = palette[p - 40];
		} else if (p >= 90 && p <= 97) {
			term.fg = palette[p - 90 + 8];
		} else if (p >= 100 && p <= 107) {
			term.bg = palette[p - 100 + 8];
		} else if ((p == 38 || p == 48) && i + 2 < term.nparams && term.params[i + 1] == 5) {
			uint32_t col = palette[term.params[i + 2] & 0xFF];
			if (p == 38) term.fg = col; else term.bg = col;
			i += 2;
		} else if ((p == 38 || p == 48) && i + 4 < term.nparams && term.params[i + 1] == 2) {
			uint32_t col = (term.params[i + 2] << 16) |
					(term.params[i + 3] << 8) | term.params[i + 4];
			if (p == 38) term.fg = col; else term.bg = col;
			i += 4;
		}
	}
}

// ---------------------------------------------------------------------------
// CSI 分派。PP(i, def)：第 i 个参数，缺省/为 0 时取 def
// ---------------------------------------------------------------------------
#define PP(i, def) ((i) < term.nparams && term.params[(i)] > 0 ? term.params[(i)] : (def))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static void term_csi(uint8_t f) {
	int n;
	switch (f) {
	case 'A':  // 光标上
		term.cy = CLAMP(term.cy - PP(0, 1), 0, term.rows - 1);
		break;
	case 'B': case 'e':  // 光标下
		term.cy = CLAMP(term.cy + PP(0, 1), 0, term.rows - 1);
		break;
	case 'C': case 'a':  // 光标右
		term.cx = CLAMP(term.cx + PP(0, 1), 0, term.cols - 1);
		term.wrap_pending = false;
		break;
	case 'D':  // 光标左
		term.cx = CLAMP(term.cx - PP(0, 1), 0, term.cols - 1);
		term.wrap_pending = false;
		break;
	case 'E':
		term.cy = CLAMP(term.cy + PP(0, 1), 0, term.rows - 1);
		term.cx = 0;
		term.wrap_pending = false;
		break;
	case 'F':
		term.cy = CLAMP(term.cy - PP(0, 1), 0, term.rows - 1);
		term.cx = 0;
		term.wrap_pending = false;
		break;
	case 'G': case '`':  // 移到列
		term.cx = CLAMP(PP(0, 1) - 1, 0, term.cols - 1);
		term.wrap_pending = false;
		break;
	case 'd':  // 移到行
		term.cy = CLAMP(PP(0, 1) - 1, 0, term.rows - 1);
		break;
	case 'H': case 'f':  // 移到 (行,列)
		term.cy = CLAMP(PP(0, 1) - 1, 0, term.rows - 1);
		term.cx = CLAMP(PP(1, 1) - 1, 0, term.cols - 1);
		term.wrap_pending = false;
		break;
	case 'J':  // 清屏
		n = PP(0, 0);
		if (n == 0) {
			clear_row(term.cy, term.cx, term.cols - 1);
			clear_rows(term.cy + 1, term.rows - 1);
		} else if (n == 1) {
			clear_rows(0, term.cy - 1);
			clear_row(term.cy, 0, term.cx);
		} else {
			clear_rows(0, term.rows - 1);
		}
		break;
	case 'K':  // 清行
		n = PP(0, 0);
		if (n == 0)
			clear_row(term.cy, term.cx, term.cols - 1);
		else if (n == 1)
			clear_row(term.cy, 0, term.cx);
		else
			clear_row(term.cy, 0, term.cols - 1);
		break;
	case 'L':  // 插入行
		scroll_down(term.cy, term.scroll_bottom, PP(0, 1));
		term.cx = 0;
		term.wrap_pending = false;
		break;
	case 'M':  // 删除行
		scroll_up(term.cy, term.scroll_bottom, PP(0, 1));
		term.cx = 0;
		term.wrap_pending = false;
		break;
	case 'P': {  // 删除字符
		n = PP(0, 1);
		if (n > term.cols - term.cx)
			n = term.cols - term.cx;
		struct cell *row = &cur_grid()[term.cy * term.cols];
		memmove(&row[term.cx], &row[term.cx + n],
				(size_t)(term.cols - term.cx - n) * sizeof(struct cell));
		clear_row(term.cy, term.cols - n, term.cols - 1);
		break;
	}
	case '@': {  // 插入空白字符
		n = PP(0, 1);
		if (n > term.cols - term.cx)
			n = term.cols - term.cx;
		struct cell *row = &cur_grid()[term.cy * term.cols];
		memmove(&row[term.cx + n], &row[term.cx],
				(size_t)(term.cols - term.cx - n) * sizeof(struct cell));
		clear_row(term.cy, term.cx, term.cx + n - 1);
		break;
	}
	case 'X':  // 擦除字符（不移动后面的内容）
		n = CLAMP(PP(0, 1), 0, term.cols - term.cx);
		clear_row(term.cy, term.cx, term.cx + n - 1);
		break;
	case 'S':  // 向上滚
		scroll_up(term.scroll_top, term.scroll_bottom, PP(0, 1));
		break;
	case 'T':  // 向下滚
		scroll_down(term.scroll_top, term.scroll_bottom, PP(0, 1));
		break;
	case 'm':
		term_sgr();
		break;
	case 'r': {  // 设置滚动区域
		int top = PP(0, 1) - 1, bottom = PP(1, term.rows) - 1;
		top = CLAMP(top, 0, term.rows - 1);
		bottom = CLAMP(bottom, 0, term.rows - 1);
		if (top < bottom) {
			term.scroll_top = top;
			term.scroll_bottom = bottom;
		}
		term.cx = term.cy = 0;
		term.wrap_pending = false;
		break;
	}
	case 's':  // 保存光标
		term.saved_cx = term.cx;
		term.saved_cy = term.cy;
		break;
	case 'u':  // 恢复光标
		term.cx = term.saved_cx;
		term.cy = term.saved_cy;
		term.wrap_pending = false;
		break;
	case 'h': case 'l': {  // 模式设置/复位（只处理 ? 私有模式）
		if (!term.priv)
			break;
		bool on = (f == 'h');
		for (int i = 0; i < term.nparams; i++) {
			switch (term.params[i]) {
			case 25:
				term.cursor_visible = on;
				break;
			case 1049:
				if (on) {
					term.saved_cx = term.cx;
					term.saved_cy = term.cy;
				} else {
					term.cx = term.saved_cx;
					term.cy = term.saved_cy;
				}
				/* fall through */
			case 1047: case 47:
				term.alt_active = on;
				if (on)
					clear_rows(0, term.rows - 1);
				term.cx = term.cy = 0;
				term.wrap_pending = false;
				break;
			}
		}
		break;
	}
	}
}

// ---------------------------------------------------------------------------
// OSC：只认 0/1/2（设置窗口标题），其余丢弃
// ---------------------------------------------------------------------------
static void osc_done(void);

static void term_control(uint8_t b) {
	switch (b) {
	case '\r':
		term.cx = 0;
		term.wrap_pending = false;
		break;
	case '\n': case '\v': case '\f':
		newline();
		break;
	case '\b':
		if (term.cx > 0)
			term.cx--;
		term.wrap_pending = false;
		break;
	case '\t':
		term.cx = CLAMP((term.cx + 8) & ~7, 0, term.cols - 1);
		term.wrap_pending = false;
		break;
	}
	// BEL 等在 GROUND 状态下直接忽略
}

// 喂给模拟器一段 PTY 输出字节流（可能是不完整的 UTF-8/转义序列，状态自留）
static void term_feed(const uint8_t *d, size_t n) {
	for (size_t i = 0; i < n; i++) {
		uint8_t b = d[i];

		// 先拼未收完的 UTF-8 序列（C0 控制符 < 0x80，不会冲突）
		if (term.utf8_need > 0) {
			if ((b & 0xC0) == 0x80) {
				term.utf8_cp = (term.utf8_cp << 6) | (b & 0x3F);
				if (--term.utf8_need == 0)
					put_char(term.utf8_cp);
				continue;
			}
			term.utf8_need = 0;  // 非法序列：丢掉，按普通字节重新处理 b
		}
		if (b >= 0x80 && term.pstate == P_GROUND) {
			if (b >= 0xF0)      { term.utf8_cp = b & 0x07; term.utf8_need = 3; }
			else if (b >= 0xE0) { term.utf8_cp = b & 0x0F; term.utf8_need = 2; }
			else if (b >= 0xC2) { term.utf8_cp = b & 0x1F; term.utf8_need = 1; }
			// 0x80-0xC1 是非法起始字节，丢弃
			continue;
		}

		switch (term.pstate) {
		case P_GROUND:
			if (b == 0x1b)
				term.pstate = P_ESC;
			else if (b < 0x20 || b == 0x7f)
				term_control(b);
			else
				put_char(b);
			break;
		case P_ESC:
			term.pstate = P_GROUND;
			switch (b) {
			case '[':
				term.pstate = P_CSI;
				term.nparams = 1;
				term.params[0] = 0;
				term.priv = false;
				break;
			case ']':
				term.pstate = P_OSC;
				term.osc_len = 0;
				break;
			case '(': case ')': case '#':  // 字符集/DECALN：吞掉下一个字节
				term.pstate = P_ESC_SKIP;
				break;
			case '7':  // 保存光标
				term.saved_cx = term.cx;
				term.saved_cy = term.cy;
				break;
			case '8':  // 恢复光标
				term.cx = term.saved_cx;
				term.cy = term.saved_cy;
				term.wrap_pending = false;
				break;
			case 'D':  // IND
				newline();
				break;
			case 'M':  // RI：反向换行
				if (term.cy == term.scroll_top)
					scroll_down(term.scroll_top, term.scroll_bottom, 1);
				else if (term.cy > 0)
					term.cy--;
				term.wrap_pending = false;
				break;
			case 'E':  // NEL
				term.cx = 0;
				newline();
				break;
			case 'c': {  // RIS：全复位
				term.cx = term.cy = 0;
				term.fg = term.bg = COLOR_DEFAULT;
				term.bold = term.inverse = false;
				term.scroll_top = 0;
				term.scroll_bottom = term.rows - 1;
				clear_rows(0, term.rows - 1);
				break;
			}
			}
			break;
		case P_ESC_SKIP:
			term.pstate = P_GROUND;
			break;
		case P_CSI:
			if (b >= '0' && b <= '9') {
				term.params[term.nparams - 1] =
						term.params[term.nparams - 1] * 10 + (b - '0');
			} else if (b == ';') {
				if (term.nparams < 16)
					term.params[term.nparams++] = 0;
			} else if (b == '?') {
				term.priv = true;
			} else if (b >= 0x40 && b <= 0x7e) {
				term_csi(b);
				term.pstate = P_GROUND;
			}
			// 其他中间字节（空格、! 等）忽略
			break;
		case P_OSC:
			if (b == 0x07) {  // BEL 结束
				osc_done();
				term.pstate = P_GROUND;
			} else if (b == 0x1b) {  // ESC \ 结束：回到 ESC 态吞掉 '\'
				osc_done();
				term.pstate = P_ESC;
			} else if (term.osc_len < (int)sizeof(term.osc) - 1) {
				term.osc[term.osc_len++] = (char)b;
			}
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// 终端尺寸
// ---------------------------------------------------------------------------
static void term_init(int cols, int rows) {
	term.cols = cols;
	term.rows = rows;
	term.grid = calloc((size_t)cols * rows, sizeof(struct cell));
	term.alt = calloc((size_t)cols * rows, sizeof(struct cell));
	term.fg = term.bg = COLOR_DEFAULT;
	term.scroll_top = 0;
	term.scroll_bottom = rows - 1;
	term.cursor_visible = true;
	clear_rows(0, rows - 1);
}

// 窗口大小变化时重建网格（尽量保留主屏内容），返回是否变化
static bool term_resize(int cols, int rows) {
	if (cols == term.cols && rows == term.rows)
		return false;
	struct cell *ng = calloc((size_t)cols * rows, sizeof(struct cell));
	struct cell *na = calloc((size_t)cols * rows, sizeof(struct cell));
	struct cell b = blank_cell();
	for (int i = 0; i < cols * rows; i++)
		ng[i] = na[i] = b;
	int mc = cols < term.cols ? cols : term.cols;
	int mr = rows < term.rows ? rows : term.rows;
	for (int y = 0; y < mr; y++)
		memcpy(&ng[y * cols], &term.grid[y * term.cols],
				(size_t)mc * sizeof(struct cell));
	free(term.grid);
	free(term.alt);
	term.grid = ng;
	term.alt = na;
	term.cols = cols;
	term.rows = rows;
	term.cx = CLAMP(term.cx, 0, cols - 1);
	term.cy = CLAMP(term.cy, 0, rows - 1);
	term.scroll_top = 0;
	term.scroll_bottom = rows - 1;
	term.wrap_pending = false;
	return true;
}

// ---------------------------------------------------------------------------
// 应用状态
// ---------------------------------------------------------------------------
struct app {
	// wayland 全局对象
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct zxdg_decoration_manager_v1 *decoration_manager;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;

	// 指针状态（surface 局部坐标）
	double ptr_x, ptr_y;
	bool ptr_hover_buttons;   // 悬停在红绿灯区域（显示 ×/−/＋ 图标）

	// 窗口
	struct wl_surface *surface;
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *decoration;
	int width, height;
	struct tinyui_frame frame;
	bool closed;
	bool configured;          // 收到首个 configure 后才能提交 buffer
	char title[256];          // 标题栏文字（OSC 0/2 可改）

	// EGL / GLES
	struct wl_egl_window *egl_window;
	EGLDisplay egl_dpy;
	EGLContext egl_ctx;
	EGLSurface egl_surf;
	bool gl_ready;

	// 字体度量
	int cell_w, cell_h;
	double ascent;

	// PTY / shell
	int pty_fd;
	pid_t shell_pid;

	// xkb 键盘状态
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
};

static struct app app = {
	.width = 724, .height = 494,
	.title = "gles-term",
	.pty_fd = -1,
};

// 按窗口尺寸算出网格列/行数
static void grid_dims(int *cols, int *rows) {
	struct tinyui_box content;
	tinyui_frame_content_box(&app.frame, &content);
	*cols = (int)((content.width - 2 * TERM_PAD) / app.cell_w);
	*rows = (int)((content.height - 2 * TERM_PAD) / app.cell_h);
	*cols = CLAMP(*cols, 10, MAX_COLS);
	*rows = CLAMP(*rows, 3, MAX_ROWS);
}

// ---------------------------------------------------------------------------
// freetype 字体度量 + 字形图集
// ---------------------------------------------------------------------------
// 字形图集：把用得到的字符一次性光栅化到一张大纹理上，绘制时按码点查
// 坐标，用纹理四边形画字。终端字符集稳定，一次构建即可。
#define ATLAS_W 1024
#define ATLAS_H 1024
#define GLYPH_CACHE_SIZE 4096
#define ATLAS_PAD 1

struct glyph {
	uint32_t cp;       // 码点
	int atlas_x, atlas_y;  // 在图集中的像素位置
	int w, h;          // 字形位图尺寸
	int bearing_x, bearing_y;  // 笔画原点到字形左上角的偏移
	int advance;       // 该字形的水平推进（像素）
};

static FT_Library ft_lib;
static FT_Face ft_face;
static double font_pixel_size = FONT_SIZE;
static GLuint atlas_tex;
// 开放寻址哈希表，避免 Unicode 码点受一个很小的直接索引数组限制。
static struct glyph atlas_glyphs[GLYPH_CACHE_SIZE];
static int atlas_count;
static int atlas_cursor_x = ATLAS_PAD;
static int atlas_cursor_y = ATLAS_PAD;
static int atlas_row_h;

static struct glyph *glyph_lookup(uint32_t cp) {
	if (cp == 0)
		return NULL;
	size_t slot = (cp * 2654435761u) & (GLYPH_CACHE_SIZE - 1);
	for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
		struct glyph *g = &atlas_glyphs[slot];
		if (g->cp == cp)
			return g;
		if (g->cp == 0)
			return NULL;
		slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1);
	}
	return NULL;
}

static struct glyph *glyph_insert(struct glyph glyph) {
	size_t slot = (glyph.cp * 2654435761u) & (GLYPH_CACHE_SIZE - 1);
	for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
		struct glyph *g = &atlas_glyphs[slot];
		if (g->cp == 0 || g->cp == glyph.cp) {
			if (g->cp == 0)
				atlas_count++;
			*g = glyph;
			return g;
		}
		slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1);
	}
	return NULL;
}

// 把一个字形渲染进图集，记录其位置和度量
static struct glyph *atlas_add(FT_Face face, uint32_t cp) {
	struct glyph *cached = glyph_lookup(cp);
	if (cached)
		return cached;
	if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) ||
			!face->glyph->bitmap.buffer)
		return NULL;
	struct glyph g = {
		.cp = cp,
		.w = (int)face->glyph->bitmap.width,
		.h = (int)face->glyph->bitmap.rows,
		.bearing_x = face->glyph->bitmap_left,
		.bearing_y = face->glyph->bitmap_top,
		.advance = (int)(face->glyph->advance.x >> 6),
	};
	if (atlas_cursor_x + g.w + ATLAS_PAD > ATLAS_W) {
		atlas_cursor_x = ATLAS_PAD;
		atlas_cursor_y += atlas_row_h + ATLAS_PAD;
		atlas_row_h = 0;
	}
	if (atlas_cursor_y + g.h + ATLAS_PAD > ATLAS_H)
		return NULL;
	if (g.h > atlas_row_h)
		atlas_row_h = g.h;
	g.atlas_x = atlas_cursor_x;
	g.atlas_y = atlas_cursor_y;
	atlas_cursor_x += g.w + ATLAS_PAD;

	// 把单色位图画进图集的 (g.atlas_x, g.atlas_y) 位置。
	// freetype 输出单通道灰度位图，扩展成 RGBA（R=G=B=0, A=亮度）上传，
	// 和 GL_RGBA 纹理格式匹配。
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	size_t npix = (size_t)g.w * g.h;
	uint8_t *rgba = calloc(npix, 4);
	if (!rgba)
		return NULL;
	FT_Bitmap *bitmap = &face->glyph->bitmap;
	for (int y = 0; y < g.h; y++) {
		const uint8_t *src = bitmap->pitch >= 0
				? bitmap->buffer + (size_t)y * bitmap->pitch
				: bitmap->buffer + (size_t)(g.h - 1 - y) * (size_t)-bitmap->pitch;
		for (int x = 0; x < g.w; x++) {
			uint8_t coverage;
			if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
				coverage = (src[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
			else
				coverage = src[x];
			rgba[((size_t)y * g.w + x) * 4 + 3] = coverage;
		}
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, g.atlas_x, g.atlas_y, g.w, g.h,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	free(rgba);

	return glyph_insert(g);
}

// 从桌面设置读取等宽字体（例如 "Ubuntu Sans Mono 13"）。返回的字号是 pt，
// Wayland 逻辑像素按常见的 96 DPI 换算，和 GTK 应用的观感保持接近。
static bool desktop_font_pattern(char *pattern, size_t pattern_size) {
	FILE *fp = popen(
			"gsettings get org.gnome.desktop.interface monospace-font-name 2>/dev/null",
			"r");
	if (!fp)
		return false;
	char setting[256];
	bool ok = fgets(setting, sizeof(setting), fp) != NULL;
	pclose(fp);
	if (!ok)
		return false;

	setting[strcspn(setting, "\r\n")] = '\0';
	char *value = setting;
	if (*value == '\'' || *value == '"') {
		char quote = *value++;
		char *end_quote = strrchr(value, quote);
		if (end_quote)
			*end_quote = '\0';
	}
	char *size_text = strrchr(value, ' ');
	if (!size_text)
		return false;
	char *end;
	double point_size = strtod(size_text + 1, &end);
	if (*end != '\0' || point_size <= 0 || point_size > 72)
		return false;
	*size_text = '\0';

	// 字体名会进入下面的 fc-match shell 命令，只接受常见的安全字符。
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (!isalnum(*p) && *p != ' ' && *p != '-' && *p != '_')
			return false;
	}
	if (snprintf(pattern, pattern_size, "%s:size=%.2f", value, point_size)
			>= (int)pattern_size)
		return false;
	font_pixel_size = point_size * 96.0 / 72.0;
	return true;
}

// 找一个可用的 monospace 字体文件。优先桌面字体设置和 fontconfig，
// 再退到 fontconfig 的通用 monospace 及常见路径。
static const char *find_font(void) {
	static char path[512];
	char pattern[256] = "monospace";
	desktop_font_pattern(pattern, sizeof(pattern));
	char command[512];
	snprintf(command, sizeof(command),
			"fc-match -f '%%{file}' '%s' 2>/dev/null", pattern);
	FILE *fp = popen(command, "r");
	if (fp) {
		if (fgets(path, sizeof(path), fp)) {
			pclose(fp);
			if (path[0] == '/')
				return path;
		} else {
			pclose(fp);
		}
	}
	static const char *fallbacks[] = {
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
		"/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
		NULL,
	};
	for (int i = 0; fallbacks[i]; i++)
		if (access(fallbacks[i], R_OK) == 0)
			return fallbacks[i];
	return NULL;
}

static void measure_font(void) {
	if (FT_Init_FreeType(&ft_lib)) {
		fprintf(stderr, "freetype 初始化失败\n");
		exit(1);
	}
	const char *font = find_font();
	if (!font) {
		fprintf(stderr, "找不到 monospace 字体（apt-get install fonts-dejavu）\n");
		exit(1);
	}
	if (FT_New_Face(ft_lib, font, 0, &ft_face)) {
		fprintf(stderr, "无法加载字体 %s\n", font);
		exit(1);
	}
	if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)lround(font_pixel_size))) {
		fprintf(stderr, "无法设置字体大小 %.1fpx\n", font_pixel_size);
		exit(1);
	}

	// 用 'M' 量单元格宽高（等宽字体所有字符 advance 相同）
	FT_Load_Char(ft_face, 'M', FT_LOAD_TARGET_LIGHT);
	app.cell_w = (int)(ft_face->glyph->advance.x >> 6);
	FT_Load_Char(ft_face, 'M', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT);
	int ascent = ft_face->size->metrics.ascender >> 6;
	int descent = ft_face->size->metrics.descender >> 6;
	app.cell_h = ascent - descent;
	app.ascent = ascent;

	// 字体度量不依赖 GLES；图集必须等 EGL context current 后再创建。
}

static void atlas_init(void) {
	// 建一张 RGBA 图集纹理，预光栅化 ASCII + 常见标点。
	// 用 RGBA 而非 GL_ALPHA：现代 GLES 驱动对单通道格式支持参差，RGBA 最稳。
	glGenTextures(1, &atlas_tex);
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	uint8_t *empty = calloc((size_t)ATLAS_W * ATLAS_H, 4);
	if (!empty) {
		fprintf(stderr, "无法分配字形图集\n");
		exit(1);
	}
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, empty);
	free(empty);
	// FreeType 已经生成逐像素灰度覆盖率，1:1 绘制时不再做线性插值，
	// 避免笔画被二次滤波后显得忽粗忽细。
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	for (uint32_t c = 0x20; c < 0x7f; c++)
		atlas_add(ft_face, c);
	// 常见非 ASCII：框线/阴影字符（htop、对话框边框用）
	static const uint32_t extra[] = {
		0x2500, 0x2501, 0x2502, 0x2503, 0x250c, 0x250f, 0x2510, 0x2513,
		0x2514, 0x2517, 0x2518, 0x251b, 0x251c, 0x251f, 0x2523, 0x252b,
		0x2533, 0x253b, 0x254b, 0x2580, 0x2584, 0x2588, 0x258c, 0x2590,
		0x2591, 0x2592, 0x2593, 0
	};
	for (int i = 0; extra[i]; i++)
		atlas_add(ft_face, extra[i]);
}

// 取码点对应的字形；不在图集里就按需补一个（终端可能收到任意 Unicode）
static struct glyph *get_glyph(uint32_t cp) {
	struct glyph *g = glyph_lookup(cp);
	return g ? g : atlas_add(ft_face, cp);
}

// ---------------------------------------------------------------------------
// GLES 渲染：矩形着色器（纯色 + 圆角裁剪）+ 字形着色器（采样图集）
// ---------------------------------------------------------------------------
// 坐标系：用像素坐标 + 正交投影，避免每个图元都算 NDC。窗口左上角为原点，
// y 向下（和终端网格、wayland 表面坐标一致）。
static GLuint rect_prog, line_prog, text_prog;
static GLint rect_proj_loc, line_proj_loc, text_proj_loc, atlas_loc;
static GLuint vbo;

static GLuint compile_shader(GLenum type, const char *src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "shader 编译失败: %s\n", log);
		exit(1);
	}
	return shader;
}

static void gl_init(void) {
	app.egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, app.display, NULL);
	if (app.egl_dpy == EGL_NO_DISPLAY || !eglInitialize(app.egl_dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize 失败\n");
		exit(1);
	}
	eglBindAPI(EGL_OPENGL_ES_API);

	EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint n;
	eglChooseConfig(app.egl_dpy, config_attrs, &config, 1, &n);

	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	app.egl_ctx = eglCreateContext(app.egl_dpy, config, EGL_NO_CONTEXT, ctx_attrs);

	app.egl_window = wl_egl_window_create(app.surface, app.width, app.height);
	app.egl_surf = eglCreateWindowSurface(app.egl_dpy, config, app.egl_window, NULL);
	eglMakeCurrent(app.egl_dpy, app.egl_surf, app.egl_surf, app.egl_ctx);
	atlas_init();

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // 预乘 alpha 混合

	// 矩形着色器：传入像素坐标顶点，fragment 里对到圆角距离做平滑裁剪。
	// u_color 是预乘后的 RGBA。圆角通过 signed-distance-field 算 alpha：取到
	// 矩形中心框的距离，减去圆角半径，负值在内部。对边缘做 fwidth 抗锯齿。
	// （不用 gl_VertexID：GLES2/GLSL ES 1.00 不支持，改用 VBO 传顶点。）
	static const char *rect_vs =
		"attribute vec2 a_pos;\n"     // 矩形四角的像素坐标
		"uniform mat4 u_proj;\n"
		"varying vec2 v_pos;\n"
		"void main() {\n"
		"  v_pos = a_pos;\n"
		"  gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);\n"
		"}\n";
	static const char *rect_fs =
		"#extension GL_OES_standard_derivatives : enable\n"  // fwidth 在 GLSL ES 1.00 需显式启用
		"precision mediump float;\n"
		"uniform vec4 u_color;\n"     // 预乘 RGBA
		"uniform vec4 u_rect;\n"      // (x, y, w, h)
		"uniform float u_radius;\n"
		"varying vec2 v_pos;\n"
		"void main() {\n"
		"  if (u_radius <= 0.0) { gl_FragColor = u_color; return; }\n"
		"  // 到矩形中心框的距离，r 为圆角半径\n"
		"  vec2 center = u_rect.xy + u_rect.zw * 0.5;\n"
		"  vec2 d = max(abs(v_pos - center) - (u_rect.zw * 0.5 - vec2(u_radius)), 0.0);\n"
		"  float dist = length(d) - u_radius;\n"
		"  float aa = fwidth(dist);\n"
		"  float a = clamp(0.5 - dist / aa, 0.0, 1.0);\n"
		"  gl_FragColor = u_color * a;\n"
		"}\n";
	rect_prog = glCreateProgram();
	glAttachShader(rect_prog, compile_shader(GL_VERTEX_SHADER, rect_vs));
	glAttachShader(rect_prog, compile_shader(GL_FRAGMENT_SHADER, rect_fs));
	glBindAttribLocation(rect_prog, 0, "a_pos");
	glLinkProgram(rect_prog);
	rect_proj_loc = glGetUniformLocation(rect_prog, "u_proj");

	// 圆头线段着色器：按钮图标用几何线段绘制，不受字体字形度量影响。
	static const char *line_fs =
		"#extension GL_OES_standard_derivatives : enable\n"
		"precision mediump float;\n"
		"uniform vec4 u_color;\n"
		"uniform vec2 u_start;\n"
		"uniform vec2 u_end;\n"
		"uniform float u_half_width;\n"
		"varying vec2 v_pos;\n"
		"void main() {\n"
		"  vec2 segment = u_end - u_start;\n"
		"  vec2 rel = v_pos - u_start;\n"
		"  float t = clamp(dot(rel, segment) / max(dot(segment, segment), 0.0001), 0.0, 1.0);\n"
		"  float dist = length(rel - segment * t) - u_half_width;\n"
		"  float aa = max(fwidth(dist), 0.001);\n"
		"  float a = clamp(0.5 - dist / aa, 0.0, 1.0);\n"
		"  gl_FragColor = u_color * a;\n"
		"}\n";
	line_prog = glCreateProgram();
	glAttachShader(line_prog, compile_shader(GL_VERTEX_SHADER, rect_vs));
	glAttachShader(line_prog, compile_shader(GL_FRAGMENT_SHADER, line_fs));
	glBindAttribLocation(line_prog, 0, "a_pos");
	glLinkProgram(line_prog);
	line_proj_loc = glGetUniformLocation(line_prog, "u_proj");

	// 字形着色器：四边形覆盖字形位图区域，采样图集纹理，按 u_color 染色。
	// 图集把 FreeType 灰度覆盖率存在 RGBA 纹理的 alpha 通道。
	static const char *text_vs =
		"attribute vec2 a_pos;\n"     // 窗口像素坐标
		"attribute vec2 a_uv;\n"      // 图集纹理坐标（已归一化）
		"uniform mat4 u_proj;\n"
		"varying vec2 v_uv;\n"
		"void main() {\n"
		"  v_uv = a_uv;\n"
		"  gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);\n"
		"}\n";
	static const char *text_fs =
		"precision mediump float;\n"
		"varying vec2 v_uv;\n"
		"uniform sampler2D u_atlas;\n"
		"uniform vec4 u_color;\n"     // 预乘 RGBA
		"void main() {\n"
		"  float a = texture2D(u_atlas, v_uv).a;\n"
		"  gl_FragColor = u_color * a;\n"
		"}\n";
	text_prog = glCreateProgram();
	glAttachShader(text_prog, compile_shader(GL_VERTEX_SHADER, text_vs));
	glAttachShader(text_prog, compile_shader(GL_FRAGMENT_SHADER, text_fs));
	glBindAttribLocation(text_prog, 0, "a_pos");
	glBindAttribLocation(text_prog, 1, "a_uv");
	glLinkProgram(text_prog);
	text_proj_loc = glGetUniformLocation(text_prog, "u_proj");
	atlas_loc = glGetUniformLocation(text_prog, "u_atlas");

	glGenBuffers(1, &vbo);
	app.gl_ready = true;
}

// 4x4 矩阵存成 16 个 float（列主序，和 GL 一致）
typedef struct { float m[16]; } mat4;

static mat4 ortho(float l, float r, float b, float t, float n, float f) {
	mat4 m = {0};
	m.m[0] = 2.0f / (r - l);
	m.m[5] = 2.0f / (t - b);
	m.m[10] = -2.0f / (f - n);
	m.m[12] = -(r + l) / (r - l);
	m.m[13] = -(t + b) / (t - b);
	m.m[14] = -(f + n) / (f - n);
	m.m[15] = 1.0f;
	return m;
}

// 把 0xRRGGBB + alpha 转成预乘 RGBA 归一化 float
static void color_premul(uint32_t rgb, float alpha, float out[4]) {
	float r = ((rgb >> 16) & 0xFF) / 255.0f;
	float g = ((rgb >> 8) & 0xFF) / 255.0f;
	float b = (rgb & 0xFF) / 255.0f;
	out[0] = r * alpha;
	out[1] = g * alpha;
	out[2] = b * alpha;
	out[3] = alpha;
}

// 画一个圆角矩形（预乘颜色）。r=0 即直角。
static void draw_rect(mat4 proj, float x, float y, float w, float h,
		float r, const float color[4]) {
	// 四个顶点的三角形带（与 v_pos attribute 对应）。GLES2 不支持 gl_VertexID。
	float verts[4][2] = {
		{x,     y},
		{x + w, y},
		{x,     y + h},
		{x + w, y + h},
	};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

	glUseProgram(rect_prog);
	glUniformMatrix4fv(rect_proj_loc, 1, GL_FALSE, proj.m);
	glUniform4f(glGetUniformLocation(rect_prog, "u_rect"), x, y, w, h);
	glUniform1f(glGetUniformLocation(rect_prog, "u_radius"), r);
	glUniform4f(glGetUniformLocation(rect_prog, "u_color"), color[0], color[1], color[2], color[3]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);
}

// 画一个实心圆（用圆角半径 = r 的正方形近似）
static void draw_circle(mat4 proj, float cx, float cy, float r,
		const float color[4]) {
	draw_rect(proj, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

// 画一条带抗锯齿圆头的线段，width 是完整线宽。
static void draw_line(mat4 proj, float x0, float y0, float x1, float y1,
		float width, const float color[4]) {
	float pad = width * 0.5f + 1.0f;
	float left = fminf(x0, x1) - pad;
	float top = fminf(y0, y1) - pad;
	float right = fmaxf(x0, x1) + pad;
	float bottom = fmaxf(y0, y1) + pad;
	float verts[4][2] = {
		{left,  top},
		{right, top},
		{left,  bottom},
		{right, bottom},
	};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

	glUseProgram(line_prog);
	glUniformMatrix4fv(line_proj_loc, 1, GL_FALSE, proj.m);
	glUniform2f(glGetUniformLocation(line_prog, "u_start"), x0, y0);
	glUniform2f(glGetUniformLocation(line_prog, "u_end"), x1, y1);
	glUniform1f(glGetUniformLocation(line_prog, "u_half_width"), width * 0.5f);
	glUniform4f(glGetUniformLocation(line_prog, "u_color"),
			color[0], color[1], color[2], color[3]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);
}

// 画一个字符（窗口像素坐标 (x, baseline)），按 color 染色。
// baseline 是文本基线的 y 坐标；字形位图相对基线偏移 (bearing_x, -bearing_y)。
static void draw_glyph_at(mat4 proj, uint32_t cp, float x, float baseline,
		const float color[4]) {
	struct glyph *g = get_glyph(cp);
	if (!g)
		return;
	float px = x + g->bearing_x;
	float py = baseline - g->bearing_y;
	float u0 = (float)g->atlas_x / ATLAS_W;
	float v0 = (float)g->atlas_y / ATLAS_H;
	float u1 = (float)(g->atlas_x + g->w) / ATLAS_W;
	float v1 = (float)(g->atlas_y + g->h) / ATLAS_H;
	// 两个三角形组成一个矩形：顶点 (pos.x, pos.y, uv.u, uv.v)
	float verts[6][4] = {
		{px,         py,         u0, v0},
		{px + g->w,  py,         u1, v0},
		{px,         py + g->h,  u0, v1},
		{px,         py + g->h,  u0, v1},
		{px + g->w,  py,         u1, v0},
		{px + g->w,  py + g->h,  u1, v1},
	};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

	glUseProgram(text_prog);
	glUniformMatrix4fv(text_proj_loc, 1, GL_FALSE, proj.m);
	glUniform1i(atlas_loc, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	glUniform4f(glGetUniformLocation(text_prog, "u_color"),
			color[0], color[1], color[2], color[3]);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

struct frame_painter {
	mat4 projection;
};

static void paint_frame_rect(void *data, int x, int y, int width, int height,
		float radius, const float color[4]) {
	struct frame_painter *painter = data;
	draw_rect(painter->projection, x, y, width, height, radius, color);
}

static void render(void) {
	if (!app.configured)
		return;  // xdg-shell 要求先 ack 首个 configure 才能提交 buffer
	if (!app.gl_ready)
		gl_init();

	eglMakeCurrent(app.egl_dpy, app.egl_surf, app.egl_surf, app.egl_ctx);
	glViewport(0, 0, app.width, app.height);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	// 正交投影：像素坐标 → NDC。wayland 表面 y 向下，所以 top=0, bottom=h。
	mat4 proj = ortho(0, app.width, app.height, 0, -1, 1);

	struct tinyui_box content;
	tinyui_frame_content_box(&app.frame, &content);

	// libtinyui owns the client-side frame; the terminal paints only content.
	float body[4];
	color_premul(0x17171a, 0.84f, body);
	draw_rect(proj, content.x, content.y, content.width, content.height,
		CORNER_RADIUS, body);
	struct frame_painter painter = {.projection = proj};
	tinyui_frame_paint(&app.frame, paint_frame_rect, &painter);

	// 终端网格
	struct cell *grid = cur_grid();
	for (int y = 0; y < term.rows; y++) {
		float row_y = content.y + TERM_PAD + y * app.cell_h;
		float baseline = row_y + app.ascent;
		for (int x = 0; x < term.cols; x++) {
			struct cell *c = &grid[y * term.cols + x];
			uint32_t fg = c->fg, bg = c->bg;
			if (c->flags & CELL_INVERSE) {
				uint32_t t = fg;
				fg = (bg == COLOR_DEFAULT) ? 0x1a1a24 : bg;
				bg = (t == COLOR_DEFAULT) ? 0xd9e6d9 : t;
			}
			float px = content.x + TERM_PAD + x * app.cell_w;
			// 背景色块（默认色不画，露出窗口主体）
			if (bg != COLOR_DEFAULT) {
				float bgc[4];
				color_premul(bg, 1.0f, bgc);
				draw_rect(proj, px, row_y, app.cell_w, app.cell_h, 0, bgc);
			}
			// 字符
			if (c->cp) {
				float fgc[4];
				if (fg == COLOR_DEFAULT)
					color_premul(0xd9e6d9, 1.0f, fgc);
				else
					color_premul(fg, 1.0f, fgc);
				draw_glyph_at(proj, c->cp, px, baseline, fgc);
			}
		}
	}

	// 光标方块
	if (term.cursor_visible) {
		float px = content.x + TERM_PAD + term.cx * app.cell_w;
		float py = content.y + TERM_PAD + term.cy * app.cell_h;
		float cur[4];
		color_premul(0xd9e6d9, 0.85f, cur);
		draw_rect(proj, px, py, app.cell_w, app.cell_h, 0, cur);
		struct cell *c = cell_at(term.cx, term.cy);
		if (c->cp) {
			float inv[4];
			color_premul(0x17171a, 1.0f, inv);
			draw_glyph_at(proj, c->cp, px,
					py + app.ascent, inv);
		}
	}

	// swap 即提交：wayland-egl 会把渲染结果交给合成器
	eglSwapBuffers(app.egl_dpy, app.egl_surf);

}

// ---------------------------------------------------------------------------
// OSC 完成：0/1/2 设置窗口标题
// ---------------------------------------------------------------------------
static void osc_done(void) {
	term.osc[term.osc_len] = '\0';
	char *semi = strchr(term.osc, ';');
	if (!semi)
		return;
	int code = atoi(term.osc);
	if (code != 0 && code != 1 && code != 2)
		return;
	snprintf(app.title, sizeof(app.title), "%s", semi + 1);
	if (app.toplevel)
		xdg_toplevel_set_title(app.toplevel, app.title);
	// 标题变了要重画；调用点之后统一 render()
}

// ---------------------------------------------------------------------------
// PTY：起 shell
// ---------------------------------------------------------------------------
static void spawn_shell(void) {
	int cols, rows;
	grid_dims(&cols, &rows);
	struct winsize ws = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
	};
	pid_t pid = forkpty(&app.pty_fd, NULL, NULL, &ws);
	if (pid < 0) {
		perror("forkpty");
		exit(1);
	}
	if (pid == 0) {
		// 子进程：PTY 从端已是 stdin/stdout/stderr 和控制终端
		setenv("TERM", "xterm-256color", 1);
		const char *shell = getenv("SHELL");
		if (!shell || !*shell)
			shell = "/bin/sh";
		execlp(shell, shell, (char *)NULL);
		_exit(127);
	}
	app.shell_pid = pid;
	signal(SIGCHLD, SIG_IGN);  // 自动回收，避免僵尸进程
	signal(SIGPIPE, SIG_IGN);  // shell 先走时写 PTY 不要被打死
}

static void pty_write(const char *s, size_t n) {
	if (app.pty_fd >= 0)
		(void)write(app.pty_fd, s, n);
}

// 通知 shell 窗口尺寸变了（SIGWINCH 由内核代发）
static void pty_resize(void) {
	if (app.pty_fd < 0)
		return;
	struct winsize ws = {
		.ws_row = (unsigned short)term.rows,
		.ws_col = (unsigned short)term.cols,
	};
	ioctl(app.pty_fd, TIOCSWINSZ, &ws);
}

// ---------------------------------------------------------------------------
// xdg-shell 回调
// ---------------------------------------------------------------------------
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xsurface_configure(void *data, struct xdg_surface *xsurface, uint32_t serial) {
	xdg_surface_ack_configure(xsurface, serial);
	app.configured = true;
	if (app.egl_window)
		wl_egl_window_resize(app.egl_window, app.width, app.height, 0, 0);
	int cols, rows;
	grid_dims(&cols, &rows);
	if (term_resize(cols, rows))
		pty_resize();
	render();
}

static const struct xdg_surface_listener xsurface_listener = {
	.configure = xsurface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	if (width > 0 && height > 0) {
		app.width = width;
		app.height = height;
		tinyui_frame_set_size(&app.frame, width, height);
	}
	app.frame.maximized = false;
	app.frame.active = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
			app.frame.maximized = true;
		else if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
			app.frame.active = true;
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	app.closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void decoration_configure(void *data,
		struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
	if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		fprintf(stderr, "gles-term: compositor rejected client-side decorations\n");
	}
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
	.configure = decoration_configure,
};

// ---------------------------------------------------------------------------
// 键盘输入：翻译成终端字节流写进 PTY
// ---------------------------------------------------------------------------
static void keyboard_keymap(void *data, struct wl_keyboard *kb,
		uint32_t format, int32_t fd, uint32_t size) {
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}
	app.xkb_keymap = xkb_keymap_new_from_string(app.xkb_ctx, map,
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);
	app.xkb_state = xkb_state_new(app.xkb_keymap);
}

static void keyboard_key(void *data, struct wl_keyboard *kb,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !app.xkb_state)
		return;

	xkb_keycode_t kc = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(app.xkb_state, kc);
	bool ctrl = xkb_state_mod_name_is_active(app.xkb_state,
			XKB_MOD_NAME_CTRL, XKB_STATE_EFFECTIVE) > 0;
	bool shift = xkb_state_mod_name_is_active(app.xkb_state,
			XKB_MOD_NAME_SHIFT, XKB_STATE_EFFECTIVE) > 0;

	switch (sym) {
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: pty_write("\r", 1);      return;
	case XKB_KEY_BackSpace:                     pty_write("\x7f", 1);    return;
	case XKB_KEY_Escape:                        pty_write("\x1b", 1);    return;
	case XKB_KEY_Tab: case XKB_KEY_KP_Tab:
		pty_write(shift ? "\x1b[Z" : "\t", shift ? 3 : 1);
		return;
	case XKB_KEY_Up:       pty_write("\x1b[A", 3);  return;
	case XKB_KEY_Down:     pty_write("\x1b[B", 3);  return;
	case XKB_KEY_Right:    pty_write("\x1b[C", 3);  return;
	case XKB_KEY_Left:     pty_write("\x1b[D", 3);  return;
	case XKB_KEY_Home:     pty_write("\x1b[H", 3);  return;
	case XKB_KEY_End:      pty_write("\x1b[F", 3);  return;
	case XKB_KEY_Prior:    pty_write("\x1b[5~", 4); return;  // PgUp
	case XKB_KEY_Next:     pty_write("\x1b[6~", 4); return;  // PgDn
	case XKB_KEY_Delete:   pty_write("\x1b[3~", 4); return;
	case XKB_KEY_Insert:   pty_write("\x1b[2~", 4); return;
	}

	// Ctrl+字母 → 控制字符（Ctrl+C=0x03、Ctrl+D=0x04、Ctrl+L=0x0C……）
	if (ctrl) {
		if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
			char c = (char)(sym - XKB_KEY_a + 1);
			pty_write(&c, 1);
			return;
		}
		if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {  // Ctrl+Shift+字母
			char c = (char)(sym - XKB_KEY_A + 1);
			pty_write(&c, 1);
			return;
		}
		if (sym == XKB_KEY_space)   { pty_write("\x00", 1); return; }
		if (sym == XKB_KEY_bracketleft)  { pty_write("\x1b", 1); return; }
		if (sym == XKB_KEY_backslash)    { pty_write("\x1c", 1); return; }
		if (sym == XKB_KEY_bracketright) { pty_write("\x1d", 1); return; }
	}

	char buf[8];
	int n = xkb_state_key_get_utf8(app.xkb_state, kc, buf, sizeof(buf));
	if (n > 0)
		pty_write(buf, n);
	// 不做本地回显：shell 经 PTY 打回来的输出才触发 render()
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {
	if (app.xkb_state)
		xkb_state_update_mask(app.xkb_state, depressed, latched, locked,
				0, 0, group);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *kb,
		uint32_t serial, struct wl_surface *surface) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
		int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static const struct wl_pointer_listener pointer_listener;

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
		app.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(app.keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

// ---------------------------------------------------------------------------
// 指针输入：libtinyui 统一完成标题栏、按钮和 resize 命中检测
// ---------------------------------------------------------------------------
static uint32_t resize_edge_for_part(enum tinyui_frame_part part) {
	switch (part) {
	case TINYUI_FRAME_RESIZE_TOP: return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
	case TINYUI_FRAME_RESIZE_BOTTOM: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
	case TINYUI_FRAME_RESIZE_LEFT: return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
	case TINYUI_FRAME_RESIZE_RIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
	case TINYUI_FRAME_RESIZE_TOP_LEFT: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
	case TINYUI_FRAME_RESIZE_TOP_RIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
	case TINYUI_FRAME_RESIZE_BOTTOM_LEFT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
	case TINYUI_FRAME_RESIZE_BOTTOM_RIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
	default: return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t sx, wl_fixed_t sy) {
	app.ptr_x = wl_fixed_to_double(sx);
	app.ptr_y = wl_fixed_to_double(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	if (app.ptr_hover_buttons) {
		app.ptr_hover_buttons = false;
		render();
	}
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	app.ptr_x = wl_fixed_to_double(sx);
	app.ptr_y = wl_fixed_to_double(sy);
	enum tinyui_frame_part part = tinyui_frame_hit_test(
		&app.frame, app.ptr_x, app.ptr_y);
	bool hover = part >= TINYUI_FRAME_CLOSE && part <= TINYUI_FRAME_MAXIMIZE;
	if (hover != app.ptr_hover_buttons) {
		app.ptr_hover_buttons = hover;
		render();
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	enum tinyui_frame_part part = tinyui_frame_hit_test(
		&app.frame, app.ptr_x, app.ptr_y);
	switch (part) {
	case TINYUI_FRAME_CLOSE:
		app.closed = true;
		break;
	case TINYUI_FRAME_MINIMIZE:
		xdg_toplevel_set_minimized(app.toplevel);
		break;
	case TINYUI_FRAME_MAXIMIZE:
		if (app.frame.maximized)
			xdg_toplevel_unset_maximized(app.toplevel);
		else
			xdg_toplevel_set_maximized(app.toplevel);
		break;
	case TINYUI_FRAME_TITLE:
		xdg_toplevel_move(app.toplevel, app.seat, serial);
		break;
	default: {
		uint32_t edge = resize_edge_for_part(part);
		if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE)
			xdg_toplevel_resize(app.toplevel, app.seat, serial, edge);
		break;
	}
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------
static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		app.compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		app.wm_base = wl_registry_bind(registry, name,
				&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		app.decoration_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		app.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(app.seat, &seat_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

// ---------------------------------------------------------------------------
int main(void) {
	palette_init();
	measure_font();
	tinyui_frame_init(&app.frame, app.width, app.height);
	int cols, rows;
	grid_dims(&cols, &rows);
	term_init(cols, rows);

	app.display = wl_display_connect(NULL);
	if (!app.display) {
		fprintf(stderr, "无法连接 Wayland display（需要在 tinywl 里运行）\n");
		return 1;
	}
	app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	struct wl_registry *registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(app.display);

	if (!app.compositor || !app.wm_base) {
		fprintf(stderr, "compositor 缺少必要协议\n");
		return 1;
	}

	app.surface = wl_compositor_create_surface(app.compositor);
	app.xsurface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
	xdg_surface_add_listener(app.xsurface, &xsurface_listener, NULL);
	app.toplevel = xdg_surface_get_toplevel(app.xsurface);
	xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);
	if (app.decoration_manager != NULL) {
		app.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
			app.decoration_manager, app.toplevel);
		zxdg_toplevel_decoration_v1_add_listener(app.decoration,
			&decoration_listener, NULL);
		zxdg_toplevel_decoration_v1_set_mode(app.decoration,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}
	xdg_toplevel_set_title(app.toplevel, app.title);
	xdg_toplevel_set_app_id(app.toplevel, "gles-term");
	wl_surface_commit(app.surface);  // 空 commit 触发首个 configure

	spawn_shell();

	// 事件循环：同时等 Wayland 事件和 PTY 输出
	int wl_fd = wl_display_get_fd(app.display);
	while (!app.closed) {
		while (wl_display_prepare_read(app.display) != 0)
			wl_display_dispatch_pending(app.display);
		if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(app.display);
			break;
		}

		struct pollfd fds[2] = {
			{ .fd = wl_fd,       .events = POLLIN },
			{ .fd = app.pty_fd,  .events = POLLIN },
		};
		if (poll(fds, 2, -1) < 0) {
			wl_display_cancel_read(app.display);
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[0].revents & POLLIN)
			wl_display_read_events(app.display);
		else
			wl_display_cancel_read(app.display);
		wl_display_dispatch_pending(app.display);

		if (fds[1].revents & (POLLIN | POLLHUP)) {
			uint8_t buf[4096];
			ssize_t n = read(app.pty_fd, buf, sizeof(buf));
			if (n > 0) {
				term_feed(buf, (size_t)n);
				render();
			} else {
				// shell 退出了（exit / Ctrl+D）：关窗
				app.closed = true;
			}
		}
	}

	// 关掉 PTY 主端，shell 会收到 SIGHUP 跟着退出
	if (app.pty_fd >= 0)
		close(app.pty_fd);
	if (app.shell_pid > 0)
		kill(app.shell_pid, SIGHUP);
	wl_display_disconnect(app.display);
	return 0;
}
