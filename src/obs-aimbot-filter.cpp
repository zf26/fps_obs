/*
 * OBS video filter — FPS Aimbot
 * Direct screen capture for inference without recording/live streaming.
 */

#include "plugin-support.h"
#include "plugin-main.h"
#include "yolo-inference.h"
#include "screen-capture.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <process.h>

static const char *filter_get_name(void *type_data);
static void *filter_create(obs_data_t *settings, obs_source_t *source);
static void filter_destroy(void *data);
static void filter_get_defaults(obs_data_t *settings);
static obs_properties_t *filter_get_properties(void *data);
static void filter_update(void *data, obs_data_t *settings);
static void filter_video_render(void *data, gs_effect_t *effect);

static struct obs_source_info aimbot_filter_info = {
	.id = "fps_aimbot_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_width = NULL,
	.get_height = NULL,
	.get_defaults = filter_get_defaults,
	.get_properties = filter_get_properties,
	.update = filter_update,
	.activate = NULL,
	.deactivate = NULL,
	.show = NULL,
	.hide = NULL,
	.video_tick = NULL,
	.video_render = filter_video_render,
	.filter_video = NULL,
	.filter_audio = NULL,
	.enum_active_sources = NULL,
	.save = NULL,
	.load = NULL,
	.mouse_click = NULL,
	.mouse_move = NULL,
	.mouse_wheel = NULL,
	.focus = NULL,
	.key_click = NULL,
	.audio_mix = NULL,
};

void register_filter(void)
{
	obs_log(LOG_INFO, "[fps-aimbot] register_filter: calling obs_register_source_s");
	obs_register_source_s(&aimbot_filter_info, sizeof(struct obs_source_info));
	obs_log(LOG_INFO, "[fps-aimbot] register_filter: done");
}

struct aimbot_filter {
	obs_source_t *context;

	char *model_path;
	struct yolo_inference *yolo;

	/* Screen capture for inference */
	struct screen_capture capture;
	HANDLE capture_thread;
	bool capture_running;

	/* Frame counter */
	int frame_count;

	bool aim_enabled;
	bool auto_snap;
	bool triggerbot_enabled;
	float confidence_threshold;
	float nms_threshold;
	float smooth_factor;
	float snap_radius;
};

static unsigned int __stdcall capture_thread_func(void *param)
{
	struct aimbot_filter *f = (struct aimbot_filter *)param;

	obs_log(LOG_INFO, "[fps-aimbot] Capture thread started");

	while (f->capture_running) {
		/* Capture screen every 100ms (10 FPS for inference) */
		Sleep(100);

		if (!f->capture_running || !f->yolo || !yolo_inference_is_loaded(f->yolo))
			continue;

		/* Grab screen */
		if (!screen_capture_grab(&f->capture))
			continue;

		/* Run inference on captured frame */
		bool ok = yolo_inference_run(f->yolo, f->capture.buffer,
			f->capture.width, f->capture.height);

		if (ok) {
			struct detection_result result = yolo_inference_get_result(f->yolo);
			if (result.count > 0) {
				obs_log(LOG_INFO, "[fps-aimbot] Detected %d targets:", result.count);
				for (int i = 0; i < result.count; i++) {
					auto &d = result.detections[i];
					obs_log(LOG_INFO, "  [%d] %s (class=%d), conf=%.2f at (%.3f, %.3f)",
						i, d.class_name, d.class_id, d.confidence, d.x, d.y);
				}
			}
		}
	}

	obs_log(LOG_INFO, "[fps-aimbot] Capture thread stopped");
	_endthreadex(0);
	return 0;
}

static const char *filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("AimbotFilter");
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "[fps-aimbot] filter_create: ENTER");
	struct aimbot_filter *f = static_cast<aimbot_filter*>(bzalloc(sizeof(struct aimbot_filter)));
	f->context = source;
	f->model_path = NULL;
	f->yolo = NULL;
	f->capture_thread = NULL;
	f->capture_running = false;
	f->frame_count = 0;

	/* Get screen dimensions */
	int screen_w = GetSystemMetrics(SM_CXSCREEN);
	int screen_h = GetSystemMetrics(SM_CYSCREEN);
	obs_log(LOG_INFO, "[fps-aimbot] Screen size: %dx%d", screen_w, screen_h);

	/* Initialize screen capture */
	if (!screen_capture_init(&f->capture, screen_w, screen_h)) {
		obs_log(LOG_ERROR, "[fps-aimbot] Failed to initialize screen capture");
	}

	filter_update(f, settings);
	obs_log(LOG_INFO, "[fps-aimbot] filter_create: DONE");
	return f;
}

static void filter_destroy(void *data)
{
	struct aimbot_filter *f = static_cast<aimbot_filter*>(data);
	if (f) {
		/* Stop capture thread */
		if (f->capture_running) {
			f->capture_running = false;
			if (f->capture_thread) {
				WaitForSingleObject(f->capture_thread, 2000);
				CloseHandle(f->capture_thread);
				f->capture_thread = NULL;
			}
		}

		screen_capture_destroy(&f->capture);

		if (f->yolo) {
			obs_log(LOG_INFO, "[fps-aimbot] Destroying YOLO model");
			yolo_inference_destroy(f->yolo);
			f->yolo = NULL;
		}
		if (f->model_path) {
			bfree(f->model_path);
			f->model_path = NULL;
		}
		bfree(f);
	}
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct aimbot_filter *f = static_cast<aimbot_filter*>(data);
	if (!f) return;

	const char *new_model_path = obs_data_get_string(settings, "model_path");

	f->aim_enabled = obs_data_get_bool(settings, "aim_enabled");
	f->auto_snap = obs_data_get_bool(settings, "auto_snap");
	f->triggerbot_enabled = obs_data_get_bool(settings, "triggerbot_enabled");
	f->confidence_threshold = (float)obs_data_get_double(settings, "confidence_threshold");
	f->nms_threshold = (float)obs_data_get_double(settings, "nms_threshold");
	f->smooth_factor = (float)obs_data_get_double(settings, "smooth_factor");
	f->snap_radius = (float)obs_data_get_double(settings, "snap_radius");

	/* Load model if path changed */
	if (new_model_path && new_model_path[0]) {
		bool path_changed = !f->model_path || strcmp(f->model_path, new_model_path) != 0;
		if (path_changed) {
			obs_log(LOG_INFO, "[fps-aimbot] Model path changed to: %s", new_model_path);

			/* Stop existing capture thread */
			if (f->capture_running) {
				f->capture_running = false;
				if (f->capture_thread) {
					WaitForSingleObject(f->capture_thread, 2000);
					CloseHandle(f->capture_thread);
					f->capture_thread = NULL;
				}
			}

			/* Destroy old model */
			if (f->yolo) {
				obs_log(LOG_INFO, "[fps-aimbot] Destroying old YOLO model");
				yolo_inference_destroy(f->yolo);
				f->yolo = NULL;
			}
			if (f->model_path) {
				bfree(f->model_path);
			}
			f->model_path = bstrdup(new_model_path);

			/* Create new model */
			obs_log(LOG_INFO, "[fps-aimbot] Loading YOLO model from: %s", f->model_path);
			f->yolo = yolo_inference_create(f->model_path, f->confidence_threshold, f->nms_threshold);
			if (f->yolo) {
				if (yolo_inference_is_loaded(f->yolo)) {
					int w, h;
					yolo_inference_get_input_size(f->yolo, &w, &h);
					obs_log(LOG_INFO, "[fps-aimbot] YOLO model loaded! Input: %dx%d", w, h);

					/* Start capture thread */
					if (!f->capture_running) {
						f->capture_running = true;
						f->capture_thread = (HANDLE)_beginthreadex(
							NULL, 0, capture_thread_func, f, 0, NULL);
						if (f->capture_thread) {
							obs_log(LOG_INFO, "[fps-aimbot] Started screen capture thread");
						} else {
							obs_log(LOG_ERROR, "[fps-aimbot] Failed to start capture thread");
						}
					}
				} else {
					obs_log(LOG_ERROR, "[fps-aimbot] YOLO model failed to load");
					yolo_inference_destroy(f->yolo);
					f->yolo = NULL;
				}
			}
		}

		/* Update thresholds */
		if (f->yolo) {
			yolo_inference_set_confidence_threshold(f->yolo, f->confidence_threshold);
			yolo_inference_set_nms_threshold(f->yolo, f->nms_threshold);
		}
	}
}

static void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct aimbot_filter *f = static_cast<aimbot_filter*>(data);
	if (!f) return;

	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target) return;

	/* Passthrough: draw the target source */
	obs_source_video_render(target);
}

static obs_properties_t *filter_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, "aim_enabled", "Enable Aim Assist");
	obs_properties_add_path(
		props, "model_path",
		"Model File (.onnx)",
		OBS_PATH_FILE,
		"ONNX Model (*.onnx);;All Files (*.*)",
		NULL);
	obs_properties_add_float(props, "confidence_threshold",
		"Confidence Threshold", 0.0, 1.0, 0.01);
	obs_properties_add_float(props, "nms_threshold",
		"NMS Threshold", 0.0, 1.0, 0.01);
	obs_properties_add_float(props, "smooth_factor",
		"Smooth Factor", 0.01, 1.0, 0.01);
	obs_properties_add_float(props, "snap_radius",
		"Snap Radius", 0.0, 0.5, 0.01);
	obs_properties_add_bool(props, "auto_snap", "Auto Aim (within radius)");
	obs_properties_add_bool(props, "triggerbot_enabled", "Enable Triggerbot");

	return props;
}

static void filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "aim_enabled", true);
	obs_data_set_default_string(settings, "model_path", "");
	obs_data_set_default_double(settings, "confidence_threshold", 0.5);
	obs_data_set_default_double(settings, "nms_threshold", 0.45);
	obs_data_set_default_double(settings, "smooth_factor", 0.35);
	obs_data_set_default_double(settings, "snap_radius", 0.15);
	obs_data_set_default_bool(settings, "auto_snap", true);
	obs_data_set_default_bool(settings, "triggerbot_enabled", false);
}
