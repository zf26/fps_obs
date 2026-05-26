#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <windows.h>

// Aim target selection modes
enum aim_target_mode {
	AIM_TARGET_NEAREST,       // Target nearest enemy to cursor
	AIM_TARGET_HIGHEST_CONF,  // Target highest confidence enemy
	AIM_TARGET_CROSSHAIR,     // Target enemy closest to crosshair (screen center)
};

// Aim point modes (where to aim on the enemy bounding box)
enum aim_point_mode {
	AIM_POINT_CENTER,   // Aim at center of bounding box (default)
	AIM_POINT_HEAD,     // Aim at top of bounding box (headshot height)
	AIM_POINT_BODY,    // Aim at 60% down the box (body mass center)
};

// Aim control configuration
struct aim_config {
	bool aim_enabled;
	enum aim_target_mode target_mode;
	enum aim_point_mode point_mode;
	float smooth_factor;        // 0.0-1.0, higher = snappier
	float snap_radius;          // Auto-aim radius (normalized 0-1)
	bool auto_snap;             // Continuously aim while enemy is near cursor
	bool triggerbot_enabled;
	float triggerbot_threshold;  // Crosshair proximity threshold (0-1)
	int triggerbot_delay_ms;     // Delay before firing (ms)
	int fire_key_vk;            // Virtual key code for fire key (0 = always on)
	bool show_crosshair;
};

struct yolo_inference;

// Aim control handle
struct aim_control *aim_control_create(void);

void aim_control_destroy(struct aim_control *ac);

void aim_control_set_config(struct aim_control *ac, struct aim_config config);

struct aim_config aim_control_get_config(struct aim_control *ac);

// Attach a YOLO inference engine to this aim control.
// The timer thread will capture the screen and run inference on it.
void aim_control_attach_inference(struct aim_control *ac, struct yolo_inference *inference);

// Set the screen capture interval in ms.
// Lower values = more responsive but higher CPU usage (default: 33ms ~30fps).
void aim_control_set_capture_interval(struct aim_control *ac, int interval_ms);

// Calculate the aim point for a detection.
// Returns normalized screen coordinates (0-1).
// If point_mode is AIM_POINT_HEAD, aims at top portion of box.
// If point_mode is AIM_POINT_BODY, aims at body center.
// If point_mode is AIM_POINT_CENTER, aims at box center.
void aim_control_calculate_point(
	float det_x, float det_y,
	float det_width, float det_height,
	enum aim_point_mode mode,
	float *out_x, float *out_y);

// Squared distance between two points (normalized coords).
// Use this for comparisons — avoids expensive sqrtf.
float aim_control_distance_sq(float x1, float y1, float x2, float y2);

// Apply smooth mouse movement toward a target (normalized 0-1 coords).
void aim_control_snap_to(struct aim_control *ac, float target_x, float target_y);

// Fire a shot (for triggerbot). Sends mouse button down/up.
void aim_control_trigger_shot(struct aim_control *ac);

// Check if the fire key is currently held.
// Returns true if fire_key_vk is 0 (always on) or if the key is pressed.
bool aim_control_is_fire_key_held(struct aim_control *ac);

// Get current cursor position in normalized screen coordinates (0-1).
void aim_control_get_cursor_pos(float *x, float *y);

// Get screen dimensions in pixels (cached at creation, refresh with aim_control_refresh_screen_size).
void aim_control_get_screen_size(int *width, int *height);

// Refresh cached screen dimensions (call after display mode change).
void aim_control_refresh_screen_size(struct aim_control *ac);

// Calculate squared distance from cursor to a point (normalized coords).
// Use for comparisons — avoids expensive sqrtf.
float aim_control_distance(float cursor_x, float cursor_y, float point_x, float point_y);

// Move mouse to a specific absolute screen position (in pixels).
void aim_control_move_mouse(struct aim_control *ac, int abs_x, int abs_y);

#ifdef __cplusplus
}
#endif
