/*
 * Enemy detector coordinator: ties screen capture -> YOLO inference -> aim assist
 *
 * Thread model:
 *  - Detection thread: capture + inference + aim assist (one thread per detector)
 *  - Render thread:    reads frame data + detection results for OBS source draw (video_tick)
 *
 * Synchronization: double-buffered frame data (ping-pong) + atomic ready flag.
 * The detection thread produces into buf[write_idx]; the render thread consumes from buf[1-write_idx].
 * This avoids holding a lock while the render thread reads.
 */

#include "enemy-detector.h"
#include "screen-capture.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct enemy_detector {
	struct screen_capture *capture;
	struct yolo_inference *inference;
	struct aim_control *aim;

	struct detector_config config;
	struct detection_result last_result;

	HANDLE thread;
	CRITICAL_SECTION lock;
	bool running;
	bool started;

	int frame_width;
	int frame_height;
	HWND target_hwnd;

	/* High-precision timer for FPS throttling */
	LARGE_INTEGER perf_freq;

	DWORD last_shot_time;

	/*
	 * Double-buffered frame data — eliminates lock contention between
	 * the detection thread (writer) and the render thread (reader).
	 * Each buffer holds full frame data. The writer writes to buf[write_idx]
	 * and flips the index; the reader reads from buf[1-write_idx].
	 * Only the index swap needs atomicity; data reads are safe because
	 * each buffer is only written by one thread.
	 */
	struct {
		uint8_t *data;
		size_t size;
		int stride;
	} frame_buf[2];
	volatile LONG frame_buf_write_idx;  /* writer index: 0 or 1 */
	volatile LONG frame_buf_ready;       /* 1 = writer finished this buffer, reader can copy */
	uint8_t *frame_copy;                /* reader's local copy (locked by OBS render thread) */

	/*
	 * Center-crop buffer for inference input (crop to model input size).
	 * Only accessed by detection thread — no sync needed.
	 */
	uint8_t *crop_buffer;
	size_t crop_buffer_size;

	/*
	 * Cached letterbox params — recomputed only when frame size changes.
	 * Only accessed by detection thread — no sync needed.
	 */
	struct {
		float scale;
		int new_w;
		int new_h;
		int pad_x;
		int pad_y;
		int last_w;
		int last_h;
	} letterbox;

	/* Per-instance log counter (not static — multi-instance safe) */
	int log_counter;
};

static DWORD WINAPI detection_thread_func(LPVOID param)
{
	struct enemy_detector *ed = (struct enemy_detector *)param;
	obs_log(LOG_INFO, "Detection thread started");

	while (ed->running) {
		LARGE_INTEGER tick_start;
		QueryPerformanceCounter(&tick_start);

		/* Skip if master switch is disabled */
		if (!ed->config.enabled) {
			Sleep((DWORD)ed->config.inference_interval_ms);
			continue;
		}

		/* 1. Capture a frame */
		if (!screen_capture_capture(ed->capture)) {
			if (screen_capture_needs_recreate(ed->capture)) {
				obs_log(LOG_WARNING, "DXGI access lost, attempting to recreate capture...");
				for (int retry = 0; retry < 20; retry++) {
					Sleep(500);
					if (!ed->running) break;
					if (screen_capture_recreate(ed->capture)) {
						obs_log(LOG_INFO, "Screen capture recreated successfully after %d retries", retry + 1);
						break;
					}
					obs_log(LOG_WARNING, "Recreate attempt %d failed, retrying...", retry + 1);
				}
			}
			Sleep((DWORD)ed->config.inference_interval_ms);
			continue;
		}

		struct capture_frame frame = screen_capture_get_frame(ed->capture);

		if (frame.data && frame.width > 0 && frame.height > 0) {
			int infer_w = frame.width;
			int infer_h = frame.height;
			const uint8_t *infer_data = frame.data;

			/* Update cached letterbox params only when frame size changes */
			if (frame.width != ed->letterbox.last_w || frame.height != ed->letterbox.last_h) {
				if (yolo_inference_is_loaded(ed->inference)) {
					int model_w, model_h;
					yolo_inference_get_input_size(ed->inference, &model_w, &model_h);
					int crop_w = ed->config.model_input_width > 0
						? ed->config.model_input_width : model_w;
					int crop_h = ed->config.model_input_height > 0
						? ed->config.model_input_height : model_h;

					if (frame.width > crop_w || frame.height > crop_h) {
						ed->letterbox.scale = (float)crop_w / frame.width;
						if ((float)frame.height * ed->letterbox.scale > crop_h)
							ed->letterbox.scale = (float)crop_h / frame.height;

						ed->letterbox.new_w = (int)(frame.width * ed->letterbox.scale);
						ed->letterbox.new_h = (int)(frame.height * ed->letterbox.scale);
						ed->letterbox.pad_x = (crop_w - ed->letterbox.new_w) / 2;
						ed->letterbox.pad_y = (crop_h - ed->letterbox.new_h) / 2;
					} else {
						ed->letterbox.scale = 1.0f;
						ed->letterbox.new_w = frame.width;
						ed->letterbox.new_h = frame.height;
						ed->letterbox.pad_x = 0;
						ed->letterbox.pad_y = 0;
					}
				}
				ed->letterbox.last_w = frame.width;
				ed->letterbox.last_h = frame.height;
			}

			/* 2. Center crop if needed (now uses cached letterbox params) */
			if (yolo_inference_is_loaded(ed->inference) &&
				(frame.width > ed->config.model_input_width ||
				 frame.height > ed->config.model_input_height)) {
				int model_w, model_h;
				yolo_inference_get_input_size(ed->inference, &model_w, &model_h);
				int crop_w = ed->config.model_input_width > 0
					? ed->config.model_input_width : model_w;
				int crop_h = ed->config.model_input_height > 0
					? ed->config.model_input_height : model_h;

				if (frame.width > crop_w || frame.height > crop_h) {
					infer_w = crop_w;
					infer_h = crop_h;
					size_t needed = (size_t)crop_w * crop_h * 4;
					if (!ed->crop_buffer || ed->crop_buffer_size < needed) {
						free(ed->crop_buffer);
						ed->crop_buffer = malloc(needed);
						ed->crop_buffer_size = needed;
					}
					int crop_x = (frame.width - crop_w) / 2;
					int crop_y = (frame.height - crop_h) / 2;

					/* Single memcpy per row — compiler can auto-vectorize */
					uint8_t *dst = ed->crop_buffer;
					const uint8_t *src_row = frame.data + (crop_y * frame.width + crop_x) * 4;
					int row_bytes = crop_w * 4;
					for (int y = 0; y < crop_h; y++) {
						memcpy(dst, src_row, row_bytes);
						dst += row_bytes;
						src_row += frame.width * 4;
					}
					infer_data = ed->crop_buffer;
				}
			}

			LARGE_INTEGER infer_start;
			QueryPerformanceCounter(&infer_start);
			bool infer_ok = yolo_inference_run(
				ed->inference, infer_data, infer_w, infer_h);
			LARGE_INTEGER infer_end;
			QueryPerformanceCounter(&infer_end);

			if (infer_ok) {
				float infer_ms = (float)(infer_end.QuadPart - infer_start.QuadPart) * 1000.0f
					/ (float)ed->perf_freq.QuadPart;
				struct detection_result result = yolo_inference_get_result(ed->inference);

				/* Per-instance log counter — not static, multi-instance safe */
				ed->log_counter++;
				if ((ed->log_counter % 150) == 0) {
					obs_log(LOG_INFO, "Inference: %.1fms, %d objects", infer_ms, result.count);
					int show = result.count < 5 ? result.count : 5;
					for (int i = 0; i < show; i++) {
						obs_log(LOG_INFO,
							"  [%d] class=%d (%s) conf=%.3f at (%.2f, %.2f)",
							i, result.detections[i].class_id,
							result.detections[i].class_name,
							result.detections[i].confidence,
							result.detections[i].x,
							result.detections[i].y);
					}
				}

				/* Update results — atomic write, no lock needed */
				ed->last_result = result;
				ed->frame_width = frame.width;
				ed->frame_height = frame.height;

				/* Double-buffer: write into buf[write_idx], then flip */
				int write_idx = ed->frame_buf_write_idx;
				int read_idx = 1 - write_idx;

				size_t needed = (size_t)frame.stride * frame.height;
				if (!ed->frame_buf[write_idx].data ||
				    ed->frame_buf[write_idx].size < needed) {
					free(ed->frame_buf[write_idx].data);
					ed->frame_buf[write_idx].data = malloc(needed);
					ed->frame_buf[write_idx].size = needed;
				}
				if (ed->frame_buf[write_idx].data) {
					memcpy(ed->frame_buf[write_idx].data, frame.data, needed);
					ed->frame_buf[write_idx].stride = frame.stride;
				}

				/* Signal reader, then swap index */
				InterlockedExchange(&ed->frame_buf_ready, 1);
				write_idx = InterlockedExchange(&ed->frame_buf_write_idx, read_idx);

				/* 3. Aim assist (runs in detection thread) */
				if (ed->config.aim_enabled && result.count > 0) {
					if (!aim_control_is_fire_key_held(ed->aim)) {
						goto skip_aim;
					}

					float cursor_x, cursor_y;
					aim_control_get_cursor_pos(&cursor_x, &cursor_y);

					float target_x = 0.5f, target_y = 0.5f;
					float best_dist_sq = 999.0f * 999.0f;
					float best_conf = 0.0f;
					bool found_target = false;

					for (int i = 0; i < result.count; i++) {
						struct detection *det = &result.detections[i];
						float aim_x, aim_y;
						aim_control_calculate_point(
							det->x, det->y,
							det->width, det->height,
							ed->config.aim_config.point_mode,
							&aim_x, &aim_y);

						switch (ed->config.aim_config.target_mode) {
						case AIM_TARGET_NEAREST: {
							float dist_sq = aim_control_distance_sq(
								cursor_x, cursor_y, aim_x, aim_y);
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
						case AIM_TARGET_CROSSHAIR: {
							float dist_sq = aim_control_distance_sq(
								0.5f, 0.5f, aim_x, aim_y);
							if (dist_sq < best_dist_sq) {
								best_dist_sq = dist_sq;
								target_x = aim_x;
								target_y = aim_y;
								found_target = true;
							}
							break;
						}
						}
					}

					if (found_target) {
						if (ed->config.aim_config.auto_snap) {
							if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
								if (best_dist_sq <= ed->config.aim_config.snap_radius *
									ed->config.aim_config.snap_radius) {
									aim_control_snap_to(ed->aim, target_x, target_y);
								}
							}
						}

						/* Triggerbot */
						if (ed->config.aim_config.triggerbot_enabled) {
							float crosshair_dist_sq = aim_control_distance_sq(
								cursor_x, cursor_y, target_x, target_y);
							if (crosshair_dist_sq <= ed->config.aim_config.triggerbot_threshold *
								ed->config.aim_config.triggerbot_threshold) {
								DWORD now = GetTickCount();
								if (now - ed->last_shot_time >
								    (DWORD)ed->config.aim_config.triggerbot_delay_ms) {
									aim_control_trigger_shot(ed->aim);
									ed->last_shot_time = now;
								}
							}
						}
					}
skip_aim:
					;
				}
			}
		}

		/* Precise throttling */
		LARGE_INTEGER tick_end;
		QueryPerformanceCounter(&tick_end);
		float elapsed_ms = (float)(tick_end.QuadPart - tick_start.QuadPart) * 1000.0f
				   / (float)ed->perf_freq.QuadPart;
		int sleep_ms = (int)(ed->config.inference_interval_ms - elapsed_ms);
		if (sleep_ms > 0) {
			Sleep((DWORD)sleep_ms);
		}
	}

	obs_log(LOG_INFO, "Detection thread stopped");
	return 0;
}

struct enemy_detector *enemy_detector_create(void)
{
	struct enemy_detector *ed = calloc(1, sizeof(struct enemy_detector));
	if (!ed) return NULL;

	ed->aim = aim_control_create();
	InitializeCriticalSection(&ed->lock);
	QueryPerformanceFrequency(&ed->perf_freq);

	ed->frame_buf[0].data = NULL;
	ed->frame_buf[0].size = 0;
	ed->frame_buf[1].data = NULL;
	ed->frame_buf[1].size = 0;
	ed->frame_copy = NULL;

	/* Default config */
	ed->config.confidence_threshold = 0.5f;
	ed->config.nms_threshold = 0.45f;
	ed->config.inference_interval_ms = 66;
	ed->config.window_capture_mode = 0;
	ed->config.target_window_title[0] = '\0';
	ed->config.monitor_idx = 0;
	ed->config.model_input_width = 0;
	ed->config.model_input_height = 0;
	ed->config.enabled = true;
	ed->config.aim_enabled = true;

	ed->config.aim_config.aim_enabled = true;
	ed->config.aim_config.target_mode = AIM_TARGET_NEAREST;
	ed->config.aim_config.point_mode = AIM_POINT_CENTER;
	ed->config.aim_config.smooth_factor = 0.35f;
	ed->config.aim_config.snap_radius = 0.15f;
	ed->config.aim_config.auto_snap = true;
	ed->config.aim_config.triggerbot_enabled = false;
	ed->config.aim_config.triggerbot_threshold = 0.03f;
	ed->config.aim_config.triggerbot_delay_ms = 50;
	ed->config.aim_config.fire_key_vk = 0;
	ed->config.aim_config.show_crosshair = true;

	ed->last_shot_time = 0;
	ed->log_counter = 0;
	ed->letterbox.last_w = -1;  /* force recompute on first frame */
	ed->letterbox.last_h = -1;

	return ed;
}

bool enemy_detector_start(struct enemy_detector *ed, struct detector_config config)
{
	if (!ed || ed->started) return false;

	ed->config = config;

	/* Find target window if window capture mode */
	if (config.window_capture_mode == 1 && strlen(config.target_window_title) > 0) {
		ed->target_hwnd = FindWindowA(NULL, config.target_window_title);
		if (!ed->target_hwnd) {
			ed->target_hwnd = FindWindowA(NULL, NULL);
			char title[256];
			while (ed->target_hwnd) {
				GetWindowTextA(ed->target_hwnd, title, sizeof(title));
				if (strstr(title, config.target_window_title) != NULL) break;
				ed->target_hwnd = GetWindow(ed->target_hwnd, GW_HWNDNEXT);
			}
		}
		if (ed->target_hwnd) {
			obs_log(LOG_INFO, "Found target window: %s", config.target_window_title);
		} else {
			obs_log(LOG_WARNING, "Could not find window: %s, falling back to monitor capture",
				config.target_window_title);
		}
	}

	/* Create screen capture */
	ed->capture = screen_capture_create(config.monitor_idx);
	if (!ed->capture) {
		obs_log(LOG_ERROR, "Failed to create screen capture for monitor %d", config.monitor_idx);
		return false;
	}

	aim_control_set_config(ed->aim, config.aim_config);

	ed->running = true;
	ed->started = true;

	/* Load YOLO model */
	if (strlen(config.model_path) > 0) {
		ed->inference = yolo_inference_create(
			config.model_path,
			config.confidence_threshold,
			config.nms_threshold);
		if (!ed->inference) {
			obs_log(LOG_ERROR, "Failed to load YOLO model: %s", config.model_path);
		}
	}

	/* Start detection thread */
	ed->thread = CreateThread(NULL, 0, detection_thread_func, ed, 0, NULL);
	if (!ed->thread) {
		obs_log(LOG_ERROR, "Failed to create detection thread");
		ed->running = false;
		ed->started = false;
		screen_capture_destroy(ed->capture);
		ed->capture = NULL;
		return false;
	}

	obs_log(LOG_INFO, "Enemy detector started (model: %s, interval: %dms, monitor: %d, window: %d)",
		config.model_path, config.inference_interval_ms, config.monitor_idx,
		config.window_capture_mode);

	return true;
}

void enemy_detector_stop(struct enemy_detector *ed)
{
	if (!ed || !ed->started) return;

	ed->running = false;

	if (ed->thread) {
		WaitForSingleObject(ed->thread, 3000);
		CloseHandle(ed->thread);
		ed->thread = NULL;
	}

	if (ed->capture) {
		screen_capture_destroy(ed->capture);
		ed->capture = NULL;
	}

	if (ed->inference) {
		yolo_inference_destroy(ed->inference);
		ed->inference = NULL;
	}

	ed->started = false;
	obs_log(LOG_INFO, "Enemy detector stopped");
}

void enemy_detector_destroy(struct enemy_detector *ed)
{
	if (!ed) return;

	enemy_detector_stop(ed);
	DeleteCriticalSection(&ed->lock);
	aim_control_destroy(ed->aim);
	free(ed->frame_buf[0].data);
	free(ed->frame_buf[1].data);
	free(ed->frame_copy);
	free(ed->crop_buffer);
	free(ed);
}

void enemy_detector_update_config(struct enemy_detector *ed, struct detector_config config)
{
	if (!ed) return;

	bool model_changed = strcmp(ed->config.model_path, config.model_path) != 0;

	ed->config = config;
	aim_control_set_config(ed->aim, config.aim_config);

	/* Invalidate letterbox cache so it recomputes for new frame size */
	ed->letterbox.last_w = -1;
	ed->letterbox.last_h = -1;

	if (model_changed && strlen(config.model_path) > 0) {
		if (ed->inference) {
			yolo_inference_destroy(ed->inference);
			ed->inference = NULL;
		}
		ed->inference = yolo_inference_create(
			config.model_path,
			config.confidence_threshold,
			config.nms_threshold);
	} else if (ed->inference) {
		yolo_inference_set_confidence_threshold(ed->inference, config.confidence_threshold);
		yolo_inference_set_nms_threshold(ed->inference, config.nms_threshold);
	}
}

struct detection_result enemy_detector_get_result(struct enemy_detector *ed)
{
	struct detection_result result = {0};
	if (!ed) return result;

	/* Atomic read — no lock needed for last_result */
	result = ed->last_result;
	return result;
}

struct detector_config enemy_detector_get_config(struct enemy_detector *ed)
{
	struct detector_config config = {0};
	if (!ed) return config;

	config = ed->config;
	return config;
}

bool enemy_detector_is_running(struct enemy_detector *ed)
{
	return ed && ed->started && ed->running;
}

void enemy_detector_get_frame_size(struct enemy_detector *ed, int *width, int *height)
{
	if (ed) {
		*width = ed->frame_width;
		*height = ed->frame_height;
	} else {
		*width = 0;
		*height = 0;
	}
}

void enemy_detector_get_target_window_handle(struct enemy_detector *ed, HWND *hwnd)
{
	if (ed && hwnd) {
		*hwnd = ed->target_hwnd;
	}
}

bool enemy_detector_get_frame_data(struct enemy_detector *ed, uint8_t *dst, size_t dst_size,
	int *width, int *height, int *stride)
{
	if (!ed || !dst || !width || !height || !stride) return false;

	/* Check if detection thread has new data ready */
	if (InterlockedCompareExchange(&ed->frame_buf_ready, 0, 0) == 0) {
		return false;
	}

	EnterCriticalSection(&ed->lock);

	/* Read the index inside the lock to avoid torn reads */
	int write_idx = ed->frame_buf_write_idx;
	int read_idx = 1 - write_idx;

	*width = ed->frame_width;
	*height = ed->frame_height;
	*stride = ed->frame_buf[read_idx].stride;

	size_t copy_size = (size_t)ed->frame_buf[read_idx].stride * ed->frame_height;
	if (copy_size > dst_size) copy_size = dst_size;

	if (ed->frame_buf[read_idx].data && copy_size > 0) {
		memcpy(dst, ed->frame_buf[read_idx].data, copy_size);
	}

	LeaveCriticalSection(&ed->lock);

	return *width > 0 && *height > 0;
}
