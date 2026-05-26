/*
 * OBS video filter — FPS Aimbot
 * GDI-based screen capture for inference without recording.
 */

#pragma once

#include <windows.h>
#include <vector>

struct screen_capture {
	HDC hdc_screen;
	HDC hdc_mem;
	HBITMAP hbm_screen;
	BITMAPINFO bmi;
	int width;
	int height;
	bool initialized;
	uint8_t *buffer;
};

bool screen_capture_init(struct screen_capture *cap, int width, int height);
void screen_capture_destroy(struct screen_capture *cap);
bool screen_capture_grab(struct screen_capture *cap);
