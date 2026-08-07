#ifndef TINYWL_GENIE_H
#define TINYWL_GENIE_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct wlr_renderer;

struct tinywl_genie_animation;

struct tinywl_genie_options {
	struct wlr_renderer *renderer;
	struct wl_event_loop *event_loop;
	struct wlr_scene_tree *animation_parent;
	struct wlr_scene_node *source;
	int source_x, source_y;
	struct wlr_scene_rect **rects;
	size_t rect_count;
	struct wlr_box window;
	double target_x, target_y;
	bool minimizing;
	int duration_ms;
	void (*finished)(void *data, bool minimizing);
	void *data;
};

struct tinywl_genie_animation *tinywl_genie_start(
	const struct tinywl_genie_options *options);
void tinywl_genie_cancel(struct tinywl_genie_animation *animation);

#endif
