#include "tinyui.h"

#include <string.h>

#define BUTTON_SIZE 12

static const float title_active[4] = {0.20f, 0.20f, 0.23f, 1.0f};
static const float title_inactive[4] = {0.16f, 0.16f, 0.18f, 1.0f};
static const float border[4] = {0.08f, 0.08f, 0.09f, 1.0f};
static const float close_color[4] = {1.0f, 0.37f, 0.34f, 1.0f};
static const float minimize_color[4] = {1.0f, 0.74f, 0.18f, 1.0f};
static const float maximize_color[4] = {0.16f, 0.78f, 0.25f, 1.0f};

void tinyui_frame_init(struct tinyui_frame *frame, int width, int height) {
	memset(frame, 0, sizeof(*frame));
	frame->width = width;
	frame->height = height;
	frame->active = true;
}

void tinyui_frame_set_size(struct tinyui_frame *frame, int width, int height) {
	frame->width = width;
	frame->height = height;
}

void tinyui_frame_content_box(const struct tinyui_frame *frame,
		struct tinyui_box *box) {
	box->x = TINYUI_BORDER_WIDTH;
	box->y = TINYUI_TITLE_HEIGHT;
	box->width = frame->width - 2 * TINYUI_BORDER_WIDTH;
	box->height = frame->height - TINYUI_TITLE_HEIGHT - TINYUI_BORDER_WIDTH;
	if (box->width < 1) box->width = 1;
	if (box->height < 1) box->height = 1;
}

enum tinyui_frame_part tinyui_frame_hit_test(
		const struct tinyui_frame *frame, double x, double y) {
	bool left = x < TINYUI_BORDER_WIDTH;
	bool right = x >= frame->width - TINYUI_BORDER_WIDTH;
	bool top = y < TINYUI_BORDER_WIDTH;
	bool bottom = y >= frame->height - TINYUI_BORDER_WIDTH;
	if (top && left) return TINYUI_FRAME_RESIZE_TOP_LEFT;
	if (top && right) return TINYUI_FRAME_RESIZE_TOP_RIGHT;
	if (bottom && left) return TINYUI_FRAME_RESIZE_BOTTOM_LEFT;
	if (bottom && right) return TINYUI_FRAME_RESIZE_BOTTOM_RIGHT;
	if (top) return TINYUI_FRAME_RESIZE_TOP;
	if (bottom) return TINYUI_FRAME_RESIZE_BOTTOM;
	if (left) return TINYUI_FRAME_RESIZE_LEFT;
	if (right) return TINYUI_FRAME_RESIZE_RIGHT;

	if (y < TINYUI_TITLE_HEIGHT) {
		static const double centers[3] = {18.0, 38.0, 58.0};
		for (int i = 0; i < 3; i++) {
			double dx = x - centers[i];
			double dy = y - TINYUI_TITLE_HEIGHT * 0.5;
			if (dx * dx + dy * dy <= 9.0 * 9.0) {
				return (enum tinyui_frame_part)(TINYUI_FRAME_CLOSE + i);
			}
		}
		return TINYUI_FRAME_TITLE;
	}
	return TINYUI_FRAME_CONTENT;
}

void tinyui_frame_paint(const struct tinyui_frame *frame,
		tinyui_fill_rect_func_t fill_rect, void *data) {
	const float *title = frame->active ? title_active : title_inactive;
	fill_rect(data, 0, 0, frame->width, TINYUI_TITLE_HEIGHT, 0, title);
	fill_rect(data, 0, 0, frame->width, TINYUI_BORDER_WIDTH, 0, border);
	fill_rect(data, 0, 0, TINYUI_BORDER_WIDTH, frame->height, 0, border);
	fill_rect(data, frame->width - TINYUI_BORDER_WIDTH, 0,
		TINYUI_BORDER_WIDTH, frame->height, 0, border);
	fill_rect(data, 0, frame->height - TINYUI_BORDER_WIDTH,
		frame->width, TINYUI_BORDER_WIDTH, 0, border);
	int button_y = TINYUI_TITLE_HEIGHT / 2 - BUTTON_SIZE / 2;
	fill_rect(data, 12, button_y, BUTTON_SIZE, BUTTON_SIZE,
		BUTTON_SIZE / 2.0f, close_color);
	fill_rect(data, 32, button_y, BUTTON_SIZE, BUTTON_SIZE,
		BUTTON_SIZE / 2.0f, minimize_color);
	fill_rect(data, 52, button_y, BUTTON_SIZE, BUTTON_SIZE,
		BUTTON_SIZE / 2.0f, maximize_color);
}
