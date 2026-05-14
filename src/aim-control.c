/*
 * FPS aim assist: smooth mouse aiming, recoil compensation, triggerbot
 */

#include "aim-control.h"
#include "plugin-support.h"

#include <windows.h>
#include <math.h>
#include <stdlib.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(x) ((void)(x))
#endif

struct aim_control {
	struct aim_config config;
	float last_target_x;
	float last_target_y;
	int cached_screen_w;
	int cached_screen_h;
};

struct aim_control *aim_control_create(void)
{
	struct aim_control *ac = calloc(1, sizeof(struct aim_control));
	if (!ac) return NULL;

	ac->config.aim_enabled = true;
	ac->config.target_mode = AIM_TARGET_NEAREST;
	ac->config.point_mode = AIM_POINT_CENTER;
	ac->config.smooth_factor = 0.35f;
	ac->config.snap_radius = 0.15f;
	ac->config.auto_snap = true;
	ac->config.triggerbot_enabled = false;
	ac->config.triggerbot_threshold = 0.03f;
	ac->config.triggerbot_delay_ms = 50;
	ac->config.fire_key_vk = 0;
	ac->config.show_crosshair = true;

	ac->last_target_x = 0.5f;
	ac->last_target_y = 0.5f;

	ac->cached_screen_w = GetSystemMetrics(SM_CXSCREEN);
	ac->cached_screen_h = GetSystemMetrics(SM_CYSCREEN);

	return ac;
}

void aim_control_destroy(struct aim_control *ac)
{
	free(ac);
}

void aim_control_set_config(struct aim_control *ac, struct aim_config config)
{
	if (ac) ac->config = config;
}

struct aim_config aim_control_get_config(struct aim_control *ac)
{
	if (!ac) {
		struct aim_config empty = {0};
		return empty;
	}
	return ac->config;
}

void aim_control_calculate_point(
	float det_x, float det_y,
	float det_width, float det_height,
	enum aim_point_mode mode,
	float *out_x, float *out_y)
{
	*out_x = det_x;
	switch (mode) {
	case AIM_POINT_CENTER:
		*out_y = det_y;
		break;
	case AIM_POINT_HEAD:
		*out_y = det_y - det_height * 0.35f;
		break;
	case AIM_POINT_BODY:
		*out_y = det_y + det_height * 0.1f;
		break;
	}
}

float aim_control_distance_sq(float x1, float y1, float x2, float y2)
{
	float dx = x1 - x2;
	float dy = y1 - y2;
	return dx * dx + dy * dy;
}

float aim_control_nearest_to_crosshair(
	const float *det_x, const float *det_y,
	int det_count,
	float crosshair_x, float crosshair_y)
{
	float best_dist_sq = 999.0f * 999.0f;
	for (int i = 0; i < det_count; i++) {
		float dist_sq = aim_control_distance_sq(
			det_x[i], det_y[i], crosshair_x, crosshair_y);
		if (dist_sq < best_dist_sq) best_dist_sq = dist_sq;
	}
	return sqrtf(best_dist_sq);
}

void aim_control_snap_to(struct aim_control *ac, float target_x, float target_y)
{
	if (!ac) return;

	POINT cursor;
	GetCursorPos(&cursor);

	int screen_w = ac->cached_screen_w;
	int screen_h = ac->cached_screen_h;

	int target_abs_x = (int)(target_x * screen_w);
	int target_abs_y = (int)(target_y * screen_h);

	float dx = (float)(target_abs_x - cursor.x);
	float dy = (float)(target_abs_y - cursor.y);
	float dist = sqrtf(dx * dx + dy * dy);

	if (dist < 1.0f) return;

	float t = ac->config.smooth_factor;
	t = fmaxf(0.05f, fminf(1.0f, t));
	float one_minus_t = 1.0f - t;
	float ease_t = 1.0f - one_minus_t * one_minus_t * one_minus_t;

	float jitter_scale = fminf(1.0f, dist / 200.0f);
	int jitter_x = (int)(((float)(rand() % 7) - 3.0f) * jitter_scale);
	int jitter_y = (int)(((float)(rand() % 7) - 3.0f) * jitter_scale);

	int new_x = cursor.x + (int)(dx * ease_t) + jitter_x;
	int new_y = cursor.y + (int)(dy * ease_t) + jitter_y;

	new_x = max(0, min(screen_w - 1, new_x));
	new_y = max(0, min(screen_h - 1, new_y));

	INPUT input = {0};
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)((new_x * 65535) / screen_w);
	input.mi.dy = (LONG)((new_y * 65535) / screen_h);
	input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

	SendInput(1, &input, sizeof(INPUT));
}

void aim_control_trigger_shot(struct aim_control *ac)
{
	UNUSED_PARAMETER(ac);

	INPUT inputs[2] = {0};

	inputs[0].type = INPUT_MOUSE;
	inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

	inputs[1].type = INPUT_MOUSE;
	inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

	SendInput(2, inputs, sizeof(INPUT));
}

bool aim_control_is_fire_key_held(struct aim_control *ac)
{
	if (!ac) return false;

	if (ac->config.fire_key_vk == 0) return true;

	SHORT state = GetAsyncKeyState(ac->config.fire_key_vk);
	return (state & 0x8000) != 0;
}

void aim_control_get_cursor_pos(float *x, float *y)
{
	POINT cursor;
	GetCursorPos(&cursor);

	int screen_w = GetSystemMetrics(SM_CXSCREEN);
	int screen_h = GetSystemMetrics(SM_CYSCREEN);

	*x = (float)cursor.x / screen_w;
	*y = (float)cursor.y / screen_h;
}

void aim_control_get_screen_size(int *width, int *height)
{
	*width = GetSystemMetrics(SM_CXSCREEN);
	*height = GetSystemMetrics(SM_CYSCREEN);
}

void aim_control_refresh_screen_size(struct aim_control *ac)
{
	if (!ac) return;
	ac->cached_screen_w = GetSystemMetrics(SM_CXSCREEN);
	ac->cached_screen_h = GetSystemMetrics(SM_CYSCREEN);
}

float aim_control_distance(float cursor_x, float cursor_y, float point_x, float point_y)
{
	float dx = cursor_x - point_x;
	float dy = cursor_y - point_y;
	return sqrtf(dx * dx + dy * dy);
}

void aim_control_move_mouse(struct aim_control *ac, int abs_x, int abs_y)
{
	UNUSED_PARAMETER(ac);

	int screen_w = GetSystemMetrics(SM_CXSCREEN);
	int screen_h = GetSystemMetrics(SM_CYSCREEN);

	INPUT input = {0};
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)((abs_x * 65535) / screen_w);
	input.mi.dy = (LONG)((abs_y * 65535) / screen_h);
	input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

	SendInput(1, &input, sizeof(INPUT));
}
