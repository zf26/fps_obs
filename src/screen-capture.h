#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct screen_capture;

// Pixel buffer for captured frame
struct capture_frame {
	uint8_t *data;  // RGBA pixel data
	int width;
	int height;
	int stride;     // bytes per row
};

// Create a screen capture instance for the given monitor (0 = primary)
struct screen_capture *screen_capture_create(int monitor_idx);

// Destroy screen capture instance
void screen_capture_destroy(struct screen_capture *sc);

// Capture the next frame. Returns true if a new frame was captured.
// The frame data is valid until the next call to screen_capture_capture().
// Returns false if no new frame is available or on error.
bool screen_capture_capture(struct screen_capture *sc);

// Get the most recently captured frame
// Returns NULL if no frame has been captured yet
struct capture_frame screen_capture_get_frame(struct screen_capture *sc);

// Get the display dimensions
void screen_capture_get_size(struct screen_capture *sc, int *width, int *height);

// Get the monitor count
int screen_capture_monitor_count(void);

// Get the display monitor name (e.g. "DELL S2721QS") for the given index
// Returns false if the name could not be determined
bool screen_capture_get_monitor_name(int monitor_idx, char *name, size_t name_size);

// Check if the capture needs recreating (access lost)
bool screen_capture_needs_recreate(struct screen_capture *sc);

// Recreate the DXGI output duplication after access lost
bool screen_capture_recreate(struct screen_capture *sc);

#ifdef __cplusplus
}
#endif
