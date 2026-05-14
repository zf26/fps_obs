#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>

struct enemy_detector;

struct aimbot_source {
	obs_source_t *source;

	int tex_width;
	int tex_height;

	bool show_overlay;

	// Rendered frame texture
	gs_texture_t *frame_tex;
	gs_texture_t *white_tex; // 1x1 white fallback when frame_tex is NULL
	uint8_t *frame_buffer;
	size_t frame_buffer_size;
};
