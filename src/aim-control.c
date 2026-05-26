/*
 * FPS aim assist: screen capture + YOLO inference + smooth mouse aiming
 *
 * Architecture: the timer thread runs completely independently of OBS.
 * Every tick_interval_ms it captures the screen, runs YOLO inference,
 * selects the best target, and moves the mouse — never touching OBS APIs.
 */

#include "aim-control.h"
#include "yolo-inference.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <windows.h>
#include <math.h>
#include <stdlib.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(x) ((void)(x))
#endif

struct aim_control {
	/* Config (set from OBS thread, read from timer thread) */
	struct aim_config config;
	CRITICAL_SECTION cs;

	/* Inference engine (set from OBS thread, used on timer thread) */
	struct yolo_inference *inference;

	/* Config-ready event: timer thread waits for this before processing.
	 * Prevents reading uninitialized config values. */
	HANDLE config_ready_event;

	/* High-resolution timer thread.
	 * Runs independently of OBS pipeline threads to avoid blocking. */
	HANDLE timer_thread;
	HANDLE stop_event;
	int tick_interval_ms;   /* interval between ticks in ms */
	int capture_interval_ms; /* interval between screen captures in ms */
	int tick_count;
};

static void capture_and_aim(struct aim_control *ac);

static DWORD WINAPI aim_control_timer_thread(LPVOID param)
{
	struct aim_control *ac = (struct aim_control *)param;

	/* Wait for config to be set before processing anything.
	 * This prevents reading uninitialized config values. */
	WaitForSingleObject(ac->config_ready_event, INFINITE);

	while (1) {
		DWORD wait2 = WaitForSingleObject(ac->stop_event, ac->tick_interval_ms);
		if (wait2 == WAIT_OBJECT_0)
			break;
		ac->tick_count++;
		capture_and_aim(ac);
	}
	return 0;
}

struct aim_control *aim_control_create(void)
{
	obs_log(LOG_INFO, "aim_control_create: ENTER");
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

	ac->tick_interval_ms = 8;
	ac->capture_interval_ms = 33;
	ac->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	ac->config_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&ac->cs);

	/* NOTE: timer thread NOT started here to avoid GDI/OBS GPU conflict.
	 * It will be started lazily on first filter activation. */

	return ac;
}

void aim_control_destroy(struct aim_control *ac)
{
	if (!ac) return;

	if (ac->timer_thread) {
		SetEvent(ac->stop_event);
		WaitForSingleObject(ac->timer_thread, INFINITE);
		CloseHandle(ac->timer_thread);
	}
	if (ac->stop_event)
		CloseHandle(ac->stop_event);
	if (ac->config_ready_event)
		CloseHandle(ac->config_ready_event);
	DeleteCriticalSection(&ac->cs);

	free(ac);
}

void aim_control_set_config(struct aim_control *ac, struct aim_config config)
{
	if (!ac) return;

	EnterCriticalSection(&ac->cs);
	ac->config = config;
	LeaveCriticalSection(&ac->cs);

	/* Signal that config is ready so the timer thread can begin processing. */
	SetEvent(ac->config_ready_event);
}

struct aim_config aim_control_get_config(struct aim_control *ac)
{
	struct aim_config empty = {0};
	if (!ac) return empty;

	EnterCriticalSection(&ac->cs);
	struct aim_config result = ac->config;
	LeaveCriticalSection(&ac->cs);

	return result;
}

void aim_control_attach_inference(struct aim_control *ac, struct yolo_inference *inference)
{
	if (!ac) return;

	EnterCriticalSection(&ac->cs);
	ac->inference = inference;
	LeaveCriticalSection(&ac->cs);
}

void aim_control_set_capture_interval(struct aim_control *ac, int interval_ms)
{
	if (!ac) return;
	ac->capture_interval_ms = interval_ms;
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

void aim_control_snap_to(struct aim_control *ac, float target_x, float target_y)
{
	if (!ac) return;

	POINT cursor;
	GetCursorPos(&cursor);

	int screen_w = GetSystemMetrics(SM_CXSCREEN);
	int screen_h = GetSystemMetrics(SM_CYSCREEN);

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
	UNUSED_PARAMETER(ac);
	/* Screen size is fetched on demand; nothing to cache for the new design */
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

/* ------------------------------------------------------------
 * Timer thread: screen capture + inference + aim
 * ------------------------------------------------------------ */

static void capture_and_aim(struct aim_control *ac)
{
	if (!ac) return;

	/* Snapshot config (timer thread-safe) */
	EnterCriticalSection(&ac->cs);
	bool aim_enabled = ac->config.aim_enabled;
	bool auto_snap = ac->config.auto_snap;
	enum aim_target_mode target_mode = ac->config.target_mode;
	enum aim_point_mode point_mode = ac->config.point_mode;
	float snap_radius = ac->config.snap_radius;
	bool triggerbot_enabled = ac->config.triggerbot_enabled;
	float triggerbot_threshold = ac->config.triggerbot_threshold;
	int triggerbot_delay_ms = ac->config.triggerbot_delay_ms;
	struct yolo_inference *inference = ac->inference;
	LeaveCriticalSection(&ac->cs);

	if (!aim_enabled) return;
	if (!inference) return;
	if (!yolo_inference_is_loaded(inference)) return;

	/* Capture the screen using GDI */
	int screen_w = GetSystemMetrics(SM_CXSCREEN);
	int screen_h = GetSystemMetrics(SM_CYSCREEN);

	HDC hdc_screen = GetDC(NULL);
	if (!hdc_screen) return;

	HDC hdc_mem = CreateCompatibleDC(hdc_screen);
	HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, screen_w, screen_h);
	if (!hbm) {
		ReleaseDC(NULL, hdc_screen);
		return;
	}
	HGDIOBJ old_bm = SelectObject(hdc_mem, hbm);
	BitBlt(hdc_mem, 0, 0, screen_w, screen_h, hdc_screen, 0, 0, SRCCOPY);
	SelectObject(hdc_mem, old_bm);

	/* Convert GDI bitmap to RGBA buffer */
	BITMAPINFOHEADER bih = {0};
	bih.biSize = sizeof(BITMAPINFOHEADER);
	bih.biWidth = screen_w;
	bih.biHeight = -screen_h; /* top-down */
	bih.biPlanes = 1;
	bih.biBitCount = 32;
	bih.biCompression = BI_RGB;

	uint8_t *rgba_buf = (uint8_t *)malloc((size_t)screen_w * screen_h * 4);
	if (!rgba_buf) {
		DeleteObject(hbm);
		DeleteDC(hdc_mem);
		ReleaseDC(NULL, hdc_screen);
		return;
	}

	GetDIBits(hdc_mem, hbm, 0, screen_h, rgba_buf, (BITMAPINFO *)&bih, DIB_RGB_COLORS);

	DeleteObject(hbm);
	DeleteDC(hdc_mem);
	ReleaseDC(NULL, hdc_screen);

	/* Run YOLO inference on the captured screen */
	bool ok = yolo_inference_run(inference, rgba_buf, screen_w, screen_h);
	free(rgba_buf);

	if (!ok) return;

	struct detection_result result = yolo_inference_get_result(inference);
	if (result.count == 0) return;

	/* Get current cursor and check fire key */
	float cursor_x, cursor_y;
	aim_control_get_cursor_pos(&cursor_x, &cursor_y);
	bool fire_key_held = aim_control_is_fire_key_held(ac);

	/* Select best target */
	float target_x = 0.5f, target_y = 0.5f;
	float best_dist_sq = 999.0f * 999.0f;
	float best_conf = 0.0f;
	bool found_target = false;

	for (int i = 0; i < result.count && i < 100; i++) {
		struct detection *det = &result.detections[i];

		float aim_x, aim_y;
		aim_control_calculate_point(det->x, det->y, det->width, det->height,
			point_mode, &aim_x, &aim_y);

		switch (target_mode) {
		case AIM_TARGET_NEAREST: {
			float dist_sq = aim_control_distance_sq(cursor_x, cursor_y, aim_x, aim_y);
			if (dist_sq < best_dist_sq) {
				best_dist_sq = dist_sq;
				target_x = aim_x;
				target_y = aim_y;
				found_target = true;
			}
			break;
		}
		case AIM_TARGET_HIGHEST_CONF:
			if (det->confidence > best_conf) {
				best_conf = det->confidence;
				target_x = aim_x;
				target_y = aim_y;
				found_target = true;
			}
			break;
		case AIM_TARGET_CROSSHAIR:
			/* Crosshair is at screen center */
			if (screen_w > 0 && screen_h > 0) {
				float cross_x = 0.5f;
				float cross_y = 0.5f;
				float dist_sq = aim_control_distance_sq(cross_x, cross_y, aim_x, aim_y);
				if (dist_sq < best_dist_sq) {
					best_dist_sq = dist_sq;
					target_x = aim_x;
					target_y = aim_y;
					found_target = true;
				}
			}
			break;
		}
	}

	if (!found_target) return;

	/* Apply snap */
	if (auto_snap && fire_key_held) {
		float dist_sq = aim_control_distance_sq(cursor_x, cursor_y, target_x, target_y);
		if (dist_sq <= snap_radius * snap_radius) {
			aim_control_snap_to(ac, target_x, target_y);
		}
	}

	/* Triggerbot */
	if (triggerbot_enabled && fire_key_held) {
		float crosshair_dist_sq = aim_control_distance_sq(cursor_x, cursor_y, target_x, target_y);
		if (crosshair_dist_sq <= triggerbot_threshold * triggerbot_threshold) {
			aim_control_trigger_shot(ac);
		}
	}
}
