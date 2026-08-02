// gles-term —— GLES 渲染的真终端（Wayland 客户端）。
//   · forkpty() 起 $SHELL：键盘输入写进 PTY，shell 输出读回来解析
//   · 内置最小 VT 模拟器：UTF-8、CSI（光标/清屏/滚动/插入删除）、
//     SGR 16/256/真彩色、备用屏幕（less/htop 可用）、OSC 标题
//   · 文字/标题栏由 cairo 在 CPU 光栅化成位图，上传为 GL 纹理
//   · GPU (GLES2 + EGL) 只负责把纹理贴上屏幕（eglSwapBuffers 自动提交）
//   · mac 风标题栏：红绿灯按钮（纯装饰）+ 居中标题（跟随 OSC 0/2）
//   · 圆角 + 半透明背景（靠像素的 alpha 通道，合成器负责混合）
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
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
#include <cairo.h>

#include "xdg-shell-client-protocol.h"

#define FONT_SIZE 16.0
#define TITLEBAR_H 30.0
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
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;

	// 指针状态（surface 局部坐标，与 cairo 坐标一致）
	double ptr_x, ptr_y;
	bool ptr_hover_buttons;   // 悬停在红绿灯区域（显示 ×/−/＋ 图标）

	// 窗口
	struct wl_surface *surface;
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;
	int width, height;
	bool closed;
	bool configured;          // 收到首个 configure 后才能提交 buffer
	char title[256];          // 标题栏文字（OSC 0/2 可改）

	// EGL / GLES
	struct wl_egl_window *egl_window;
	EGLDisplay egl_dpy;
	EGLContext egl_ctx;
	EGLSurface egl_surf;
	GLuint gl_prog;
	GLuint gl_tex;
	bool gl_ready;
	bool gl_has_bgra;

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
	.width = 720, .height = 460,
	.title = "gles-term",
	.pty_fd = -1,
};

// 按窗口尺寸算出网格列/行数
static void grid_dims(int *cols, int *rows) {
	*cols = (int)((app.width - 2 * TERM_PAD) / app.cell_w);
	*rows = (int)((app.height - TITLEBAR_H - 2 * TERM_PAD) / app.cell_h);
	*cols = CLAMP(*cols, 10, MAX_COLS);
	*rows = CLAMP(*rows, 3, MAX_ROWS);
}

static void measure_font(void) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(s);
	cairo_select_font_face(cr, "monospace",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE);
	cairo_text_extents_t te;
	cairo_text_extents(cr, "M", &te);
	app.cell_w = (int)ceil(te.width);
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	app.cell_h = (int)ceil(fe.height);
	app.ascent = fe.ascent;
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

// ---------------------------------------------------------------------------
// cairo 绘制整窗内容（标题栏 + 红绿灯 + 终端网格 + 光标）到一张 CPU 位图
// ---------------------------------------------------------------------------
static void rounded_rect_path(cairo_t *cr, double x, double y,
		double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r,     r, -M_PI_2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
	cairo_arc(cr, x + r,     y + h - r, r, M_PI_2, M_PI);
	cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI_2);
	cairo_close_path(cr);
}

static void draw_circle(cairo_t *cr, double cx, double cy, double r,
		double red, double green, double blue) {
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, red, green, blue);
	cairo_fill(cr);
}

static void set_fg_color(cairo_t *cr, uint32_t rgb, double alpha) {
	if (rgb == COLOR_DEFAULT)
		cairo_set_source_rgba(cr, 0.85, 0.9, 0.85, alpha);
	else
		cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0,
				((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0, alpha);
}

static void set_bg_color(cairo_t *cr, uint32_t rgb) {
	if (rgb == COLOR_DEFAULT)
		cairo_set_source_rgb(cr, 0.16, 0.16, 0.20);  // 反色时当"纸"用
	else
		cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0,
				((rgb >> 8) & 0xFF) / 255.0, (rgb & 0xFF) / 255.0);
}

static size_t utf8_encode(uint32_t cp, char *out) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = 0xC0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3F);
		return 2;
	} else if (cp < 0x10000) {
		out[0] = 0xE0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F);
		return 3;
	}
	out[0] = 0xF0 | (cp >> 18);
	out[1] = 0x80 | ((cp >> 12) & 0x3F);
	out[2] = 0x80 | ((cp >> 6) & 0x3F);
	out[3] = 0x80 | (cp & 0x3F);
	return 4;
}

// 画一个单元格的字符（光标反显用）
static void draw_glyph(cairo_t *cr, uint32_t cp, double x, double baseline) {
	char buf[8];
	size_t n = utf8_encode(cp ? cp : ' ', buf);
	buf[n] = '\0';
	cairo_move_to(cr, x, baseline);
	cairo_show_text(cr, buf);
}

static void draw_grid(cairo_t *cr) {
	cairo_select_font_face(cr, "monospace",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE);

	struct cell *g = cur_grid();
	for (int y = 0; y < term.rows; y++) {
		double baseline = TITLEBAR_H + TERM_PAD + app.ascent + y * app.cell_h;
		double row_y = TITLEBAR_H + TERM_PAD + y * app.cell_h;
		int x = 0;
		while (x < term.cols) {
			// 把连续同色的单元格攒成一串一次画，比逐字画快很多
			uint32_t fg = g[y * term.cols + x].fg;
			uint32_t bg = g[y * term.cols + x].bg;
			uint8_t fl = g[y * term.cols + x].flags;
			if (fl & CELL_INVERSE) {
				uint32_t t = fg;
				fg = (bg == COLOR_DEFAULT) ? 0x1a1a24 : bg;
				bg = (t == COLOR_DEFAULT) ? 0xd9e6d9 : t;
			}
			int x0 = x;
			char run[MAX_COLS * 4 + 1];
			size_t runlen = 0;
			while (x < term.cols) {
				struct cell *c = &g[y * term.cols + x];
				uint32_t cf = c->fg, cb = c->bg;
				if (c->flags & CELL_INVERSE) {
					uint32_t t = cf;
					cf = (cb == COLOR_DEFAULT) ? 0x1a1a24 : cb;
					cb = (t == COLOR_DEFAULT) ? 0xd9e6d9 : t;
				}
				if (cf != fg || cb != bg)
					break;
				runlen += utf8_encode(c->cp ? c->cp : ' ', run + runlen);
				x++;
			}
			run[runlen] = '\0';
			double px = TERM_PAD + x0 * app.cell_w;
			double pw = (x - x0) * app.cell_w;
			if (bg != COLOR_DEFAULT) {
				cairo_rectangle(cr, px, row_y, pw, app.cell_h);
				set_bg_color(cr, bg);
				cairo_fill(cr);
			}
			set_fg_color(cr, fg, 1.0);
			cairo_move_to(cr, px, baseline);
			cairo_show_text(cr, run);
		}
	}

	// 光标方块
	if (term.cursor_visible) {
		double px = TERM_PAD + term.cx * app.cell_w;
		double py = TITLEBAR_H + TERM_PAD + term.cy * app.cell_h;
		cairo_rectangle(cr, px, py, app.cell_w, app.cell_h);
		cairo_set_source_rgba(cr, 0.85, 0.9, 0.85, 0.85);
		cairo_fill(cr);
		struct cell *c = cell_at(term.cx, term.cy);
		if (c->cp && c->cp != ' ') {
			cairo_set_source_rgb(cr, 0.09, 0.09, 0.12);
			draw_glyph(cr, c->cp, px,
					TITLEBAR_H + TERM_PAD + app.ascent + term.cy * app.cell_h);
		}
	}
}

static void rasterize(uint32_t *pixels, int w, int h) {
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
			(uint8_t *)pixels, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_t *cr = cairo_create(cs);

	// 全透明底（圆角外的区域靠它露出后面的东西）
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	// 后续绘制必须回到 OVER（叠加），否则每层都会整体替换掉上一层
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	// 圆角矩形裁剪
	rounded_rect_path(cr, 0, 0, w, h, CORNER_RADIUS);
	cairo_clip(cr);

	// 窗口主体：半透明深色（透出后面的桌面壁纸）
	cairo_set_source_rgba(cr, 0.09, 0.09, 0.12, 0.82);
	cairo_paint(cr);

	// 标题栏：略亮、略不透明
	cairo_rectangle(cr, 0, 0, w, TITLEBAR_H);
	cairo_set_source_rgba(cr, 0.20, 0.20, 0.23, 0.95);
	cairo_fill(cr);
	// 标题栏下沿分隔线
	cairo_rectangle(cr, 0, TITLEBAR_H - 1, w, 1);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
	cairo_fill(cr);

	// 红绿灯按钮（mac 顺序：红=关闭 黄=最小化 绿=最大化）
	double cy = TITLEBAR_H / 2, r = 6;
	static const double btn_x[3] = { 20, 40, 60 };
	draw_circle(cr, btn_x[0], cy, r, 1.000, 0.373, 0.341);  // #FF5F57
	draw_circle(cr, btn_x[1], cy, r, 0.996, 0.737, 0.180);  // #FEBC2E
	draw_circle(cr, btn_x[2], cy, r, 0.157, 0.784, 0.251);  // #28C840

	// 悬停时显示 ×/−/＋ 图标（mac 行为：悬停任意一个，三个都显示）
	if (app.ptr_hover_buttons) {
		static const char *glyphs[3] = { "×", "−", "+" };
		cairo_select_font_face(cr, "sans",
				CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 9);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
		for (int i = 0; i < 3; i++) {
			cairo_text_extents_t ge;
			cairo_text_extents(cr, glyphs[i], &ge);
			cairo_move_to(cr, btn_x[i] - ge.width / 2 - ge.x_bearing,
					cy - ge.height / 2 - ge.y_bearing);
			cairo_show_text(cr, glyphs[i]);
		}
	}

	// 居中标题（默认 gles-term；shell 通过 OSC 0/2 会改成当前命令/目录）
	cairo_select_font_face(cr, "sans",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 13);
	cairo_text_extents_t ext;
	cairo_text_extents(cr, app.title, &ext);
	cairo_move_to(cr, (w - ext.width) / 2, cy + ext.height / 2 - 1);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	cairo_show_text(cr, app.title);

	draw_grid(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// GLES 初始化与绘制
// ---------------------------------------------------------------------------
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
	// EGL_PLATFORM_WAYLAND_EXT 与 EGL_PLATFORM_WAYLAND 同值（0x31D8），
	// 老 EGL 头文件只有前者
	app.egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, app.display, NULL);
	if (app.egl_dpy == EGL_NO_DISPLAY || !eglInitialize(app.egl_dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize 失败\n");
		exit(1);
	}
	eglBindAPI(EGL_OPENGL_ES_API);

	// 必须带 alpha 通道，半透明才能生效
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

	// 纹理着色器：全屏四边形贴一张图
	static const char *vs_src =
		"attribute vec2 pos;\n"
		"attribute vec2 tex;\n"
		"varying vec2 v_tex;\n"
		"void main() { v_tex = tex; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs_src =
		"precision mediump float;\n"
		"varying vec2 v_tex;\n"
		"uniform sampler2D image;\n"
		"void main() { gl_FragColor = texture2D(image, v_tex); }\n";
	app.gl_prog = glCreateProgram();
	glAttachShader(app.gl_prog, compile_shader(GL_VERTEX_SHADER, vs_src));
	glAttachShader(app.gl_prog, compile_shader(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(app.gl_prog, 0, "pos");
	glBindAttribLocation(app.gl_prog, 1, "tex");
	glLinkProgram(app.gl_prog);
	glUseProgram(app.gl_prog);

	glGenTextures(1, &app.gl_tex);
	glBindTexture(GL_TEXTURE_2D, app.gl_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// cairo 输出在小端机器上是 BGRA 内存序，有扩展就不用自己换位
	const char *exts = (const char *)glGetString(GL_EXTENSIONS);
	app.gl_has_bgra = exts && strstr(exts, "GL_EXT_texture_format_BGRA8888");

	app.gl_ready = true;
}

static void render(void) {
	if (!app.configured)
		return;  // xdg-shell 要求先 ack 首个 configure 才能提交 buffer
	if (!app.gl_ready)
		gl_init();

	// CPU 光栅化整窗内容
	uint32_t *pixels = malloc((size_t)app.width * app.height * 4);
	rasterize(pixels, app.width, app.height);

	eglMakeCurrent(app.egl_dpy, app.egl_surf, app.egl_surf, app.egl_ctx);
	glViewport(0, 0, app.width, app.height);
	glBindTexture(GL_TEXTURE_2D, app.gl_tex);

	if (app.gl_has_bgra) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, app.width, app.height,
				0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
	} else {
		// 没有 BGRA 扩展就手动交换 R/B 通道
		for (int i = 0; i < app.width * app.height; i++) {
			uint32_t p = pixels[i];
			pixels[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, app.width, app.height,
				0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	free(pixels);

	// NDC 全屏四边形；GL 纹理坐标原点在左下，v 轴翻转
	static const float verts[] = {
		// pos      // tex
		-1, -1,     0, 1,
		 1, -1,     1, 1,
		-1,  1,     0, 0,
		 1,  1,     1, 0,
	};
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

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
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	app.closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
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
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
		app.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

// ---------------------------------------------------------------------------
// 指针输入：标题栏拖拽移动、红钮关闭、红绿灯 hover
// ---------------------------------------------------------------------------
// 命中检测：红绿灯圆心在 (20/40/60, TITLEBAR_H/2)，点击半径给宽松一点
static int hit_traffic_button(double x, double y) {
	static const double btn_x[3] = { 20, 40, 60 };
	for (int i = 0; i < 3; i++) {
		double dx = x - btn_x[i], dy = y - TITLEBAR_H / 2;
		if (dx * dx + dy * dy <= 9 * 9)
			return i;  // 0=红 1=黄 2=绿
	}
	return -1;
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
	// hover 区域：标题栏左侧红绿灯一带
	bool hover = app.ptr_y < TITLEBAR_H && app.ptr_x < 80;
	if (hover != app.ptr_hover_buttons) {
		app.ptr_hover_buttons = hover;
		render();
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	if (app.ptr_y >= TITLEBAR_H)
		return;  // 点击正文区域：无操作

	int btn = hit_traffic_button(app.ptr_x, app.ptr_y);
	if (btn == 0) {
		app.closed = true;  // 红钮：关闭窗口
	} else if (btn < 0) {
		// 标题栏空白处：请求合成器开始交互式拖拽移动
		xdg_toplevel_move(app.toplevel, app.seat, serial);
	}
	// 黄/绿钮：纯装饰，无操作
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
