/*
 * OBS source plugin for FPS auto-aim
 * - Renders captured screen with detection overlay
 * - Provides configuration UI via OBS properties
 */

#include "obs-aimbot-source.h"
#include "enemy-detector.h"
#include "screen-capture.h"
#include "plugin-support.h"
#include "aim-control.h"

#include <graphics/graphics.h>
#include <util/platform.h>

#ifndef NDEBUG
#define obs_log_debug(log_level, ...) obs_log(log_level, __VA_ARGS__)
#else
#define obs_log_debug(log_level, ...) ((void)0)
#endif

// Forward declarations
static const char *aimbot_source_get_name(void *type_data);
static void *aimbot_source_create(obs_data_t *settings, obs_source_t *source);
static void aimbot_source_destroy(void *data);
static uint32_t aimbot_source_get_width(void *data);
static uint32_t aimbot_source_get_height(void *data);
static void aimbot_source_video_tick(void *data, float seconds);
static void aimbot_source_video_render(void *data, gs_effect_t *effect);
static obs_properties_t *aimbot_source_get_properties(void *data);
static void aimbot_source_get_defaults(obs_data_t *settings);
static void aimbot_source_update(void *data, obs_data_t *settings);

static struct enemy_detector *g_detector = NULL;
static int g_source_count = 0;

struct obs_source_info aimbot_source_info = {
	.id = "fps_aimbot_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = aimbot_source_get_name,
	.create = aimbot_source_create,
	.destroy = aimbot_source_destroy,
	.get_width = aimbot_source_get_width,
	.get_height = aimbot_source_get_height,
	.get_defaults = aimbot_source_get_defaults,
	.get_properties = aimbot_source_get_properties,
	.update = aimbot_source_update,
	.video_tick = aimbot_source_video_tick,
	.video_render = aimbot_source_video_render,
};

static const char *aimbot_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("AimbotSource");
}

static void *aimbot_source_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log_debug(LOG_INFO, "aimbot_source_create: creating new source instance");

	struct aimbot_source *as = bzalloc(sizeof(struct aimbot_source));
	as->source = source;
	as->show_overlay = true;
	as->frame_tex = NULL;
	as->frame_buffer = NULL;
	as->frame_buffer_size = 0;

	if (g_source_count == 0) {
		obs_log_debug(LOG_INFO, "aimbot_source_create: initializing global detector");
		g_detector = enemy_detector_create();
	}

	g_source_count++;

	aimbot_source_update(as, settings);

	obs_log(LOG_INFO, "FPS Aimbot source created (total instances: %d)", g_source_count);

	return as;
}

static void aimbot_source_destroy(void *data)
{
	struct aimbot_source *as = data;
	if (!as) return;

	g_source_count--;
	if (g_source_count <= 0) {
		if (g_detector) {
			enemy_detector_stop(g_detector);
			enemy_detector_destroy(g_detector);
			g_detector = NULL;
		}
		g_source_count = 0;
	}

	if (as->frame_tex) {
		gs_texture_destroy(as->frame_tex);
		as->frame_tex = NULL;
	}
	if (as->white_tex) {
		gs_texture_destroy(as->white_tex);
		as->white_tex = NULL;
	}
	free(as->frame_buffer);
	as->frame_buffer = NULL;
	as->frame_buffer_size = 0;

	bfree(as);

	obs_log(LOG_INFO, "FPS Aimbot source destroyed (remaining: %d)", g_source_count);
}

static uint32_t aimbot_source_get_width(void *data)
{
	UNUSED_PARAMETER(data);
	if (!g_detector) return 1920;

	int w = 0, h = 0;
	enemy_detector_get_frame_size(g_detector, &w, &h);
	return (uint32_t)(w > 0 ? w : 1920);
}

static uint32_t aimbot_source_get_height(void *data)
{
	UNUSED_PARAMETER(data);
	if (!g_detector) return 1080;

	int w = 0, h = 0;
	enemy_detector_get_frame_size(g_detector, &w, &h);
	return (uint32_t)(h > 0 ? h : 1080);
}

static void aimbot_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct aimbot_source *as = data;
	if (!as) return;

	if (!g_detector || !enemy_detector_is_running(g_detector)) {
		return;
	}

	int cap_w = 0, cap_h = 0;
	enemy_detector_get_frame_size(g_detector, &cap_w, &cap_h);

	// Allocate buffer matching capture frame size
	size_t needed = (size_t)(cap_w > 0 ? cap_w : 1920) * 4 * (size_t)(cap_h > 0 ? cap_h : 1080);
	if (!as->frame_buffer || as->frame_buffer_size < needed) {
		free(as->frame_buffer);
		as->frame_buffer_size = needed;
		as->frame_buffer = malloc(as->frame_buffer_size);
	}

	int frame_w = 0, frame_h = 0, frame_stride = 0;
	if (!enemy_detector_get_frame_data(g_detector, as->frame_buffer, as->frame_buffer_size,
		&frame_w, &frame_h, &frame_stride)) return;

	// Recreate texture if dimensions changed
	if (!as->frame_tex || as->tex_width != frame_w || as->tex_height != frame_h) {
		if (as->frame_tex) {
			gs_texture_destroy(as->frame_tex);
			as->frame_tex = NULL;
		}
		as->frame_tex = gs_texture_create(frame_w, frame_h, GS_BGRA, 1, NULL,
			GS_DYNAMIC);
		if (!as->frame_tex) return;

		as->tex_width = frame_w;
		as->tex_height = frame_h;
	}

	// Upload frame data to GPU texture
	gs_texture_set_image(as->frame_tex, as->frame_buffer, (uint32_t)frame_stride, false);
}

static void draw_bounding_box(float x, float y, float w, float h, uint32_t color, float thickness)
{
	float t = thickness / 1080.0f;

	// Top
	gs_render_start(true);
	gs_color(color); gs_vertex2f(x, y);
	gs_color(color); gs_vertex2f(x + w, y);
	gs_color(color); gs_vertex2f(x + w, y + t);
	gs_color(color); gs_vertex2f(x, y + t);
	gs_render_stop(GS_TRISTRIP);

	// Bottom
	gs_render_start(true);
	gs_color(color); gs_vertex2f(x, y + h - t);
	gs_color(color); gs_vertex2f(x + w, y + h - t);
	gs_color(color); gs_vertex2f(x + w, y + h);
	gs_color(color); gs_vertex2f(x, y + h);
	gs_render_stop(GS_TRISTRIP);

	// Left
	gs_render_start(true);
	gs_color(color); gs_vertex2f(x, y);
	gs_color(color); gs_vertex2f(x + t, y);
	gs_color(color); gs_vertex2f(x + t, y + h);
	gs_color(color); gs_vertex2f(x, y + h);
	gs_render_stop(GS_TRISTRIP);

	// Right
	gs_render_start(true);
	gs_color(color); gs_vertex2f(x + w - t, y);
	gs_color(color); gs_vertex2f(x + w, y);
	gs_color(color); gs_vertex2f(x + w, y + h);
	gs_color(color); gs_vertex2f(x + w - t, y + h);
	gs_render_stop(GS_TRISTRIP);
}

static void draw_crosshair(float cx, float cy, float size, uint32_t color)
{
	float half = size / 2.0f;

	// Horizontal line
	gs_render_start(true);
	gs_color(color); gs_vertex2f(cx - half, cy);
	gs_color(color); gs_vertex2f(cx + half, cy);
	gs_color(color); gs_vertex2f(cx + half, cy + 1.0f);
	gs_color(color); gs_vertex2f(cx - half, cy + 1.0f);
	gs_render_stop(GS_TRISTRIP);

	// Vertical line
	gs_render_start(true);
	gs_color(color); gs_vertex2f(cx, cy - half);
	gs_color(color); gs_vertex2f(cx + 1.0f, cy - half);
	gs_color(color); gs_vertex2f(cx + 1.0f, cy + half);
	gs_color(color); gs_vertex2f(cx, cy + half);
	gs_render_stop(GS_TRISTRIP);

	// Center dot
	gs_render_start(true);
	gs_color(color); gs_vertex2f(cx - 2.0f, cy - 2.0f);
	gs_color(color); gs_vertex2f(cx + 2.0f, cy - 2.0f);
	gs_color(color); gs_vertex2f(cx + 2.0f, cy + 2.0f);
	gs_color(color); gs_vertex2f(cx - 2.0f, cy + 2.0f);
	gs_render_stop(GS_TRISTRIP);
}

static uint32_t color_for_class(int class_id)
{
	switch (class_id) {
	case 0:  return 0xFFFF0000; // Red for enemies/persons
	case 2:  return 0xFF00FF00; // Green
	case 3:  return 0xFFFFFF00; // Yellow
	case 5:  return 0xFFFF8000; // Orange
	case 7:  return 0xFFFF00FF; // Magenta
	default: return 0xFFFF00FF; // Magenta
	}
}

static void draw_aim_point(float x, float y, float size, uint32_t color)
{
	float s = size / 2.0f;

	gs_render_start(true);
	// Diamond shape
	gs_color(color); gs_vertex2f(x, y - s);
	gs_color(color); gs_vertex2f(x + s, y);
	gs_color(color); gs_vertex2f(x, y + s);
	gs_color(color); gs_vertex2f(x - s, y);
	gs_render_stop(GS_TRISTRIP);
}

static void aimbot_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct aimbot_source *as = data;
	if (!as || !g_detector) return;

	if (!enemy_detector_is_running(g_detector)) {
		return;
	}

	struct detection_result result = enemy_detector_get_result(g_detector);

	int screen_w = 0, screen_h = 0;
	enemy_detector_get_frame_size(g_detector, &screen_w, &screen_h);
	if (screen_w <= 0 || screen_h <= 0) return;

	gs_matrix_push();
	gs_matrix_identity();
	gs_ortho(0.0f, (float)screen_w, (float)screen_h, 0.0f, -100.0f, 100.0f);

	// Use OBS base effect for all rendering (texture + overlays)
	gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	// Lazily create 1x1 white fallback texture
	if (!as->white_tex) {
		uint32_t white_data = 0xFFFFFFFF;
		const uint8_t *white_ptr = (const uint8_t *)&white_data;
		as->white_tex = gs_texture_create(1, 1, GS_BGRA, 1, &white_ptr, 0);
	}

	gs_eparam_t *param = gs_effect_get_param_by_name(eff, "image");
	gs_texture_t *bind_tex = as->frame_tex ? as->frame_tex : as->white_tex;
	gs_effect_set_texture(param, bind_tex);

	// Draw the captured frame as a fullscreen texture
	gs_render_start(true);
	gs_color(0xFFFFFFFF); gs_vertex2f(0.0f, 0.0f);
	gs_color(0xFFFFFFFF); gs_vertex2f((float)screen_w, 0.0f);
	gs_color(0xFFFFFFFF); gs_vertex2f((float)screen_w, (float)screen_h);
	gs_color(0xFFFFFFFF); gs_vertex2f(0.0f, (float)screen_h);
	gs_render_stop(GS_TRISTRIP);

	// Draw overlay on top
	if (as->show_overlay) {
		struct detector_config cfg = enemy_detector_get_config(g_detector);

		for (int i = 0; i < result.count; i++) {
			struct detection *det = &result.detections[i];

			float x = (det->x - det->width / 2) * screen_w;
			float y = (det->y - det->height / 2) * screen_h;
			float w = det->width * screen_w;
			float h = det->height * screen_h;
			uint32_t color = color_for_class(det->class_id);

			draw_bounding_box(x, y, w, h, color, 3.0f);

			// Draw aim point marker
			float aim_x = det->x * screen_w;
			float aim_y = det->y * screen_h;
			switch (cfg.aim_config.point_mode) {
			case AIM_POINT_HEAD:
				aim_y = (det->y - det->height * 0.35f) * screen_h;
				break;
			case AIM_POINT_BODY:
				aim_y = (det->y + det->height * 0.1f) * screen_h;
				break;
			case AIM_POINT_CENTER:
			default:
				break;
			}
			draw_aim_point(aim_x, aim_y, 8.0f, color);
		}

		// Draw crosshair at screen center
		if (cfg.aim_config.show_crosshair) {
			float cx = (float)screen_w / 2.0f;
			float cy = (float)screen_h / 2.0f;
			draw_crosshair(cx, cy, 30.0f, 0xFFFFFF00);
		}

		// Draw target aim point if there's an enemy near crosshair
		if (result.count > 0 && cfg.aim_enabled) {
			float best_x = 0.5f, best_y = 0.5f;
			float best_dist = 999.0f;
			for (int i = 0; i < result.count; i++) {
				struct detection *det = &result.detections[i];
				float dx = det->x - 0.5f;
				float dy = det->y - 0.5f;
				float dist = sqrtf(dx * dx + dy * dy);
				if (dist < best_dist) {
					best_dist = dist;
					best_x = det->x;
					best_y = det->y;
				}
			}
			if (best_dist < cfg.aim_config.snap_radius) {
				float px = best_x * screen_w;
				float py = best_y * screen_h;
				draw_aim_point(px, py, 12.0f, 0xFF00FF00);
			}
		}
	}

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_matrix_pop();
}

static bool model_path_changed(obs_properties_t *props, obs_property_t *property,
			       obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(settings);
	return true;
}

static obs_properties_t *aimbot_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	// Master enable switch
	obs_properties_add_bool(
		props, "enabled",
		obs_module_text("Enabled"));

	// Model file
	obs_property_t *p = obs_properties_add_path(
		props, "model_path",
		obs_module_text("ModelPath"),
		OBS_PATH_FILE,
		"ONNX Model (*.onnx);;All Files (*.*)",
		NULL);
	obs_property_set_modified_callback(p, model_path_changed);

	// Detection parameters
	obs_properties_add_float_slider(
		props, "confidence_threshold",
		obs_module_text("ConfidenceThreshold"),
		0.0, 1.0, 0.01);

	obs_properties_add_float_slider(
		props, "nms_threshold",
		obs_module_text("NMSThreshold"),
		0.0, 1.0, 0.01);

	obs_properties_add_int(
		props, "inference_interval",
		obs_module_text("InferenceInterval"),
		33, 500, 10);

	// Model input size override
	obs_properties_add_int(
		props, "model_input_width",
		obs_module_text("ModelInputWidth"),
		0, 4096, 32);
	obs_properties_add_int(
		props, "model_input_height",
		obs_module_text("ModelInputHeight"),
		0, 4096, 32);

	// Capture mode
	obs_property_t *cap_list = obs_properties_add_list(
		props, "capture_mode",
		obs_module_text("CaptureMode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cap_list, obs_module_text("CaptureModeMonitor"), 0);
	obs_property_list_add_int(cap_list, obs_module_text("CaptureModeWindow"), 1);

	// Monitor selection
	int monitor_count = screen_capture_monitor_count();
	if (monitor_count == 0) monitor_count = 1;

	obs_property_t *monitor_list = obs_properties_add_list(
		props, "monitor_idx",
		obs_module_text("Monitor"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	for (int i = 0; i < monitor_count; i++) {
		char name[128];
		screen_capture_get_monitor_name(i, name, sizeof(name));
		obs_property_list_add_int(monitor_list, name, i);
	}

	// Game window title
	obs_properties_add_text(
		props, "window_title",
		obs_module_text("WindowTitle"),
		OBS_TEXT_DEFAULT);

	// Aim assist
	obs_properties_add_bool(
		props, "aim_enabled",
		obs_module_text("AimEnabled"));

	// Aim target mode
	obs_property_t *aim_target_list = obs_properties_add_list(
		props, "aim_target_mode",
		obs_module_text("AimTargetMode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(aim_target_list, obs_module_text("AimTargetNearest"), AIM_TARGET_NEAREST);
	obs_property_list_add_int(aim_target_list, obs_module_text("AimTargetHighestConf"), AIM_TARGET_HIGHEST_CONF);
	obs_property_list_add_int(aim_target_list, obs_module_text("AimTargetCrosshair"), AIM_TARGET_CROSSHAIR);

	// Aim point mode
	obs_property_t *aim_point_list = obs_properties_add_list(
		props, "aim_point_mode",
		obs_module_text("AimPointMode"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(aim_point_list, obs_module_text("AimPointCenter"), AIM_POINT_CENTER);
	obs_property_list_add_int(aim_point_list, obs_module_text("AimPointHead"), AIM_POINT_HEAD);
	obs_property_list_add_int(aim_point_list, obs_module_text("AimPointBody"), AIM_POINT_BODY);

	// Smooth factor
	obs_properties_add_float_slider(
		props, "smooth_factor",
		obs_module_text("SmoothFactor"),
		0.01, 1.0, 0.01);

	// Snap radius
	obs_properties_add_float_slider(
		props, "snap_radius",
		obs_module_text("SnapRadius"),
		0.0, 0.5, 0.01);

	obs_properties_add_bool(
		props, "auto_snap",
		obs_module_text("AutoSnap"));

	// Triggerbot
	obs_properties_add_bool(
		props, "triggerbot_enabled",
		obs_module_text("TriggerbotEnabled"));

	obs_properties_add_float_slider(
		props, "triggerbot_threshold",
		obs_module_text("TriggerbotThreshold"),
		0.0, 0.2, 0.01);

	obs_properties_add_int(
		props, "triggerbot_delay",
		obs_module_text("TriggerbotDelay"),
		0, 500, 10);

	// Fire key
	obs_property_t *fire_key_list = obs_properties_add_list(
		props, "fire_key_vk",
		obs_module_text("FireKey"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyAlways"), 0);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyLeftMouse"), 1);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyRightMouse"), 2);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyShift"), 16);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyCtrl"), 17);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyAlt"), 18);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeySpace"), 32);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyX1"), 5);
	obs_property_list_add_int(fire_key_list, obs_module_text("FireKeyX2"), 6);

	// Display
	obs_properties_add_bool(
		props, "show_overlay",
		obs_module_text("ShowOverlay"));

	obs_properties_add_bool(
		props, "show_crosshair",
		obs_module_text("ShowCrosshair"));

	return props;
}

static void aimbot_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "model_path", "");
	obs_data_set_default_double(settings, "confidence_threshold", 0.5);
	obs_data_set_default_double(settings, "nms_threshold", 0.45);
	obs_data_set_default_int(settings, "inference_interval", 66);
	obs_data_set_default_int(settings, "capture_mode", 0);
	obs_data_set_default_int(settings, "monitor_idx", 0);
	obs_data_set_default_int(settings, "model_input_width", 0);
	obs_data_set_default_int(settings, "model_input_height", 0);
	obs_data_set_default_string(settings, "window_title", "");
	obs_data_set_default_bool(settings, "enabled", true);
	obs_data_set_default_bool(settings, "aim_enabled", true);
	obs_data_set_default_int(settings, "aim_target_mode", AIM_TARGET_NEAREST);
	obs_data_set_default_int(settings, "aim_point_mode", AIM_POINT_CENTER);
	obs_data_set_default_double(settings, "smooth_factor", 0.35);
	obs_data_set_default_double(settings, "snap_radius", 0.15);
	obs_data_set_default_bool(settings, "auto_snap", true);
	obs_data_set_default_bool(settings, "triggerbot_enabled", false);
	obs_data_set_default_double(settings, "triggerbot_threshold", 0.03);
	obs_data_set_default_int(settings, "triggerbot_delay", 50);
	obs_data_set_default_int(settings, "fire_key_vk", 0);
	obs_data_set_default_bool(settings, "show_overlay", true);
	obs_data_set_default_bool(settings, "show_crosshair", true);
}

static void aimbot_source_update(void *data, obs_data_t *settings)
{
	struct aimbot_source *as = data;
	if (!as) return;

	as->show_overlay = obs_data_get_bool(settings, "show_overlay");

	if (!g_detector) return;

	struct detector_config config;
	memset(&config, 0, sizeof(config));

	const char *model_path = obs_data_get_string(settings, "model_path");
	if (model_path) {
		strncpy(config.model_path, model_path, MAX_MODEL_PATH - 1);
		config.model_path[MAX_MODEL_PATH - 1] = '\0';
	}

	const char *window_title = obs_data_get_string(settings, "window_title");
	if (window_title) {
		strncpy(config.target_window_title, window_title, sizeof(config.target_window_title) - 1);
		config.target_window_title[sizeof(config.target_window_title) - 1] = '\0';
	}

	config.confidence_threshold = (float)obs_data_get_double(settings, "confidence_threshold");
	config.nms_threshold = (float)obs_data_get_double(settings, "nms_threshold");
	config.inference_interval_ms = (int)obs_data_get_int(settings, "inference_interval");
	if (config.inference_interval_ms < 33) config.inference_interval_ms = 33;
	config.window_capture_mode = (int)obs_data_get_int(settings, "capture_mode");
	config.monitor_idx = (int)obs_data_get_int(settings, "monitor_idx");
	config.model_input_width = (int)obs_data_get_int(settings, "model_input_width");
	config.model_input_height = (int)obs_data_get_int(settings, "model_input_height");
	config.enabled = obs_data_get_bool(settings, "enabled");
	config.aim_enabled = obs_data_get_bool(settings, "aim_enabled");

	config.aim_config.aim_enabled = obs_data_get_bool(settings, "aim_enabled");
	config.aim_config.target_mode = (enum aim_target_mode)(int)obs_data_get_int(settings, "aim_target_mode");
	config.aim_config.point_mode = (enum aim_point_mode)(int)obs_data_get_int(settings, "aim_point_mode");
	config.aim_config.smooth_factor = (float)obs_data_get_double(settings, "smooth_factor");
	config.aim_config.snap_radius = (float)obs_data_get_double(settings, "snap_radius");
	config.aim_config.auto_snap = obs_data_get_bool(settings, "auto_snap");
	config.aim_config.triggerbot_enabled = obs_data_get_bool(settings, "triggerbot_enabled");
	config.aim_config.triggerbot_threshold = (float)obs_data_get_double(settings, "triggerbot_threshold");
	config.aim_config.triggerbot_delay_ms = (int)obs_data_get_int(settings, "triggerbot_delay");
	config.aim_config.fire_key_vk = (int)obs_data_get_int(settings, "fire_key_vk");
	config.aim_config.show_crosshair = obs_data_get_bool(settings, "show_crosshair");

	enemy_detector_update_config(g_detector, config);

	if (strlen(config.model_path) > 0 && !enemy_detector_is_running(g_detector)) {
		enemy_detector_start(g_detector, config);
	} else if (strlen(config.model_path) == 0 && enemy_detector_is_running(g_detector)) {
		enemy_detector_stop(g_detector);
	}
}
