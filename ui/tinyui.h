#ifndef TINYUI_H
#define TINYUI_H

#include <stdbool.h>

#define TINYUI_TITLE_HEIGHT 32
#define TINYUI_BORDER_WIDTH 2

struct tinyui_box {
	int x, y, width, height;
};

enum tinyui_frame_part {
	TINYUI_FRAME_CONTENT,
	TINYUI_FRAME_TITLE,
	TINYUI_FRAME_CLOSE,
	TINYUI_FRAME_MINIMIZE,
	TINYUI_FRAME_MAXIMIZE,
	TINYUI_FRAME_RESIZE_TOP,
	TINYUI_FRAME_RESIZE_BOTTOM,
	TINYUI_FRAME_RESIZE_LEFT,
	TINYUI_FRAME_RESIZE_RIGHT,
	TINYUI_FRAME_RESIZE_TOP_LEFT,
	TINYUI_FRAME_RESIZE_TOP_RIGHT,
	TINYUI_FRAME_RESIZE_BOTTOM_LEFT,
	TINYUI_FRAME_RESIZE_BOTTOM_RIGHT,
};

typedef void (*tinyui_fill_rect_func_t)(void *data,
	int x, int y, int width, int height, float radius,
	const float color[4]);

struct tinyui_frame {
	int width, height;
	bool active;
	bool maximized;
	bool hover_buttons;
};

void tinyui_frame_init(struct tinyui_frame *frame, int width, int height);
void tinyui_frame_set_size(struct tinyui_frame *frame, int width, int height);
void tinyui_frame_content_box(const struct tinyui_frame *frame,
	struct tinyui_box *box);
enum tinyui_frame_part tinyui_frame_hit_test(
	const struct tinyui_frame *frame, double x, double y);
void tinyui_frame_paint(const struct tinyui_frame *frame,
	tinyui_fill_rect_func_t fill_rect, void *data);

#endif
