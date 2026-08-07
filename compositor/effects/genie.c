#include "genie.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>

#define GENIE_STRIPS 48

enum genie_piece_type {
	GENIE_PIECE_BUFFER,
	GENIE_PIECE_RECT,
};

struct genie_piece {
	struct wl_list link;
	enum genie_piece_type type;
	union {
		struct wlr_scene_buffer *buffer;
		struct wlr_scene_rect *rect;
	};
	double nx0, nx1, ny0, ny1;
};

struct genie_texture {
	struct wl_list link;
	struct wlr_texture *texture;
};

struct tinywl_genie_animation {
	struct tinywl_genie_options options;
	struct wlr_scene_tree *tree;
	struct wl_event_source *timer;
	struct wl_list pieces;
	struct wl_list textures;
	struct timespec started_at;
};

static double clamp01(double value) {
	return fmax(0.0, fmin(1.0, value));
}

static double smoothstep(double value) {
	value = clamp01(value);
	return value * value * (3.0 - 2.0 * value);
}

static void destroy_animation(struct tinywl_genie_animation *animation) {
	if (animation->timer != NULL) {
		wl_event_source_remove(animation->timer);
	}
	struct genie_piece *piece, *tmp;
	wl_list_for_each_safe(piece, tmp, &animation->pieces, link) {
		if (piece->type == GENIE_PIECE_BUFFER) {
			/* Slice nodes borrow textures owned by animation->textures. */
			piece->buffer->texture = NULL;
		}
		wl_list_remove(&piece->link);
		free(piece);
	}
	if (animation->tree != NULL) {
		wlr_scene_node_destroy(&animation->tree->node);
	}
	struct genie_texture *texture, *texture_tmp;
	wl_list_for_each_safe(texture, texture_tmp, &animation->textures, link) {
		wl_list_remove(&texture->link);
		wlr_texture_destroy(texture->texture);
		free(texture);
	}
	free(animation);
}

void tinywl_genie_cancel(struct tinywl_genie_animation *animation) {
	if (animation != NULL) {
		destroy_animation(animation);
	}
}

static void update_animation(struct tinywl_genie_animation *animation,
		double progress) {
	const double target_width = 14.0;
	const double target_height = 6.0;
	const double vertical_progress = smoothstep(progress);
	const struct tinywl_genie_options *options = &animation->options;
	struct genie_piece *piece;
	wl_list_for_each(piece, &animation->pieces, link) {
		double vertical_center = (piece->ny0 + piece->ny1) * 0.5;
		double delay = (1.0 - vertical_center) * 0.24;
		double side_progress = smoothstep((progress - delay) / (1.0 - delay));
		double left = options->window.x * (1.0 - side_progress) +
			(options->target_x - target_width * 0.5) * side_progress;
		double right = (options->window.x + options->window.width) *
			(1.0 - side_progress) +
			(options->target_x + target_width * 0.5) * side_progress;
		double x0 = left + (right - left) * piece->nx0;
		double x1 = left + (right - left) * piece->nx1;

		double original_y0 = options->window.y +
			options->window.height * piece->ny0;
		double original_y1 = options->window.y +
			options->window.height * piece->ny1;
		double target_y0 = options->target_y +
			(piece->ny0 - 0.5) * target_height;
		double target_y1 = options->target_y +
			(piece->ny1 - 0.5) * target_height;
		double y0 = original_y0 * (1.0 - vertical_progress) +
			target_y0 * vertical_progress;
		double y1 = original_y1 * (1.0 - vertical_progress) +
			target_y1 * vertical_progress;

		int x = (int)lround(x0);
		int y = (int)lround(y0);
		int width = (int)lround(x1 - x0);
		int height = (int)lround(y1 - y0);
		if (width < 1) width = 1;
		if (height < 1) height = 1;
		if (piece->type == GENIE_PIECE_BUFFER) {
			wlr_scene_node_set_position(&piece->buffer->node, x, y);
			wlr_scene_buffer_set_dest_size(piece->buffer, width, height);
		} else {
			wlr_scene_node_set_position(&piece->rect->node, x, y);
			wlr_scene_rect_set_size(piece->rect, width, height);
		}
	}
}

static int animation_timer(void *data) {
	struct tinywl_genie_animation *animation = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed_ms =
		(now.tv_sec - animation->started_at.tv_sec) * 1000.0 +
		(now.tv_nsec - animation->started_at.tv_nsec) / 1000000.0;
	double phase = clamp01(elapsed_ms / animation->options.duration_ms);
	double progress = animation->options.minimizing ? phase : 1.0 - phase;
	update_animation(animation, progress);
	if (phase >= 1.0) {
		void (*finished)(void *, bool) = animation->options.finished;
		void *finished_data = animation->options.data;
		bool minimizing = animation->options.minimizing;
		animation->timer = NULL;
		destroy_animation(animation);
		if (finished != NULL) {
			finished(finished_data, minimizing);
		}
		return 0;
	}
	wl_event_source_timer_update(animation->timer, 16);
	return 0;
}

static struct genie_piece *new_piece(struct tinywl_genie_animation *animation,
		double x0, double y0, double x1, double y1) {
	struct genie_piece *piece = calloc(1, sizeof(*piece));
	if (piece == NULL) {
		return NULL;
	}
	const struct wlr_box *window = &animation->options.window;
	piece->nx0 = (x0 - window->x) / window->width;
	piece->nx1 = (x1 - window->x) / window->width;
	piece->ny0 = (y0 - window->y) / window->height;
	piece->ny1 = (y1 - window->y) / window->height;
	wl_list_insert(animation->pieces.prev, &piece->link);
	return piece;
}

static void add_buffer_slices(struct wlr_scene_buffer *source,
		int sx, int sy, void *data) {
	struct tinywl_genie_animation *animation = data;
	if (source->buffer == NULL) {
		return;
	}
	struct genie_texture *snapshot = calloc(1, sizeof(*snapshot));
	if (snapshot == NULL) {
		return;
	}
	snapshot->texture = wlr_texture_from_buffer(
		animation->options.renderer, source->buffer);
	if (snapshot->texture == NULL) {
		free(snapshot);
		return;
	}
	wl_list_insert(animation->textures.prev, &snapshot->link);
	struct wlr_fbox source_box = source->src_box;
	if (source_box.width <= 0 || source_box.height <= 0) {
		source_box = (struct wlr_fbox) {
			.width = source->buffer->width,
			.height = source->buffer->height,
		};
	}
	int display_width = source->dst_width > 0 ? source->dst_width :
		(int)lround(source_box.width);
	int display_height = source->dst_height > 0 ? source->dst_height :
		(int)lround(source_box.height);
	if (display_width <= 0 || display_height <= 0) {
		return;
	}
	int strips = display_height < GENIE_STRIPS ? display_height : GENIE_STRIPS;
	for (int i = 0; i < strips; i++) {
		int y0 = i * display_height / strips;
		int y1 = (i + 1) * display_height / strips;
		int global_x = animation->options.source_x + sx;
		int global_y = animation->options.source_y + sy;
		struct genie_piece *piece = new_piece(animation,
			global_x, global_y + y0,
			global_x + display_width, global_y + y1);
		if (piece == NULL) {
			return;
		}
		struct wlr_fbox strip_box = {
			.x = source_box.x,
			.y = source_box.y + source_box.height * y0 / display_height,
			.width = source_box.width,
			.height = source_box.height * (y1 - y0) / display_height,
		};
		piece->type = GENIE_PIECE_BUFFER;
		piece->buffer = wlr_scene_buffer_create(animation->tree, NULL);
		if (piece->buffer == NULL) {
			wl_list_remove(&piece->link);
			free(piece);
			return;
		}
		piece->buffer->texture = snapshot->texture;
		wlr_scene_buffer_set_source_box(piece->buffer, &strip_box);
		wlr_scene_buffer_set_dest_size(piece->buffer, display_width, y1 - y0);
		wlr_scene_buffer_set_opacity(piece->buffer, source->opacity);
		wlr_scene_buffer_set_filter_mode(piece->buffer, source->filter_mode);
	}
}

static void add_rect_slices(struct tinywl_genie_animation *animation,
		struct wlr_scene_rect *source) {
	int sx, sy;
	/* Coordinates remain valid while an ancestor is disabled during restore. */
	wlr_scene_node_coords(&source->node, &sx, &sy);
	if (source->width <= 0 || source->height <= 0) {
		return;
	}
	int strips = source->height < GENIE_STRIPS ? source->height : GENIE_STRIPS;
	for (int i = 0; i < strips; i++) {
		int y0 = i * source->height / strips;
		int y1 = (i + 1) * source->height / strips;
		struct genie_piece *piece = new_piece(animation,
			sx, sy + y0, sx + source->width, sy + y1);
		if (piece == NULL) {
			return;
		}
		piece->type = GENIE_PIECE_RECT;
		piece->rect = wlr_scene_rect_create(animation->tree,
			source->width, y1 - y0, source->color);
		if (piece->rect == NULL) {
			wl_list_remove(&piece->link);
			free(piece);
			return;
		}
	}
}

struct tinywl_genie_animation *tinywl_genie_start(
		const struct tinywl_genie_options *options) {
	if (options == NULL || options->renderer == NULL ||
			options->event_loop == NULL || options->animation_parent == NULL ||
			options->source == NULL || options->window.width <= 0 ||
			options->window.height <= 0 || options->duration_ms <= 0) {
		return NULL;
	}
	struct tinywl_genie_animation *animation = calloc(1, sizeof(*animation));
	if (animation == NULL) {
		return NULL;
	}
	animation->options = *options;
	wl_list_init(&animation->pieces);
	wl_list_init(&animation->textures);
	animation->tree = wlr_scene_tree_create(options->animation_parent);
	if (animation->tree == NULL) {
		destroy_animation(animation);
		return NULL;
	}

	wlr_scene_node_for_each_buffer(options->source, add_buffer_slices, animation);
	for (size_t i = 0; i < options->rect_count; i++) {
		add_rect_slices(animation, options->rects[i]);
	}
	if (wl_list_empty(&animation->pieces)) {
		destroy_animation(animation);
		return NULL;
	}

	wlr_scene_node_raise_to_top(&animation->tree->node);
	clock_gettime(CLOCK_MONOTONIC, &animation->started_at);
	animation->timer = wl_event_loop_add_timer(options->event_loop,
		animation_timer, animation);
	if (animation->timer == NULL) {
		destroy_animation(animation);
		return NULL;
	}
	update_animation(animation, options->minimizing ? 0.0 : 1.0);
	wl_event_source_timer_update(animation->timer, 16);
	wlr_log(WLR_INFO, "genie.start direction=%s target=%.0f,%.0f duration_ms=%d",
		options->minimizing ? "minimize" : "restore",
		options->target_x, options->target_y, options->duration_ms);
	return animation;
}
