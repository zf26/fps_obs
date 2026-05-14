#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "yolo-inference.h"
#include "aim-control.h"
#include <stdbool.h>
#include <windows.h>

#define MAX_MODEL_PATH 512

// Configuration for the enemy detector
struct detector_config {
	char model_path[MAX_MODEL_PATH];
	float confidence_threshold;
	float nms_threshold;
	int inference_interval_ms;    // ms between inference runs
	int window_capture_mode;      // 0 = monitor (DXGI), 1 = game window (GDI fallback)
	char target_window_title[256]; // window title to capture (partial match)
	int monitor_idx;             // which monitor to capture (when mode=0)
	int model_input_width;       // override inference input width (0 = auto from model)
	int model_input_height;      // override inference input height (0 = auto from model)
	bool enabled;                 // master enable switch
	bool aim_enabled;             // enable aim assist
	struct aim_config aim_config;
};

struct enemy_detector;

struct enemy_detector *enemy_detector_create(void);

bool enemy_detector_start(struct enemy_detector *ed, struct detector_config config);

void enemy_detector_stop(struct enemy_detector *ed);

void enemy_detector_destroy(struct enemy_detector *ed);

void enemy_detector_update_config(struct enemy_detector *ed, struct detector_config config);

struct detection_result enemy_detector_get_result(struct enemy_detector *ed);

struct detector_config enemy_detector_get_config(struct enemy_detector *ed);

bool enemy_detector_is_running(struct enemy_detector *ed);

void enemy_detector_get_frame_size(struct enemy_detector *ed, int *width, int *height);

void enemy_detector_get_target_window_handle(struct enemy_detector *ed, HWND *hwnd);

// Copy the latest captured frame data into the destination buffer (thread-safe).
// dst must be large enough to hold stride * height bytes.
// Returns true if frame data was available and copied.
bool enemy_detector_get_frame_data(struct enemy_detector *ed, uint8_t *dst, size_t dst_size, int *width, int *height, int *stride);

#ifdef __cplusplus
}
#endif
