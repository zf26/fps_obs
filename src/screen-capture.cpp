/*
 * GDI-based screen capture
 * Simple and reliable, works without requiring special drivers.
 */

#include "screen-capture.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

static void log_msg(const char *level, const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	char full[576];
	snprintf(full, sizeof(full), "[screen-capture] [%s] %s\n", level, buf);
	OutputDebugStringA(full);
}

#define LOG_INFO(fmt, ...) log_msg("INFO", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg("ERROR", fmt, ##__VA_ARGS__)

bool screen_capture_init(struct screen_capture *cap, int width, int height)
{
	memset(cap, 0, sizeof(*cap));
	cap->width = width;
	cap->height = height;

	/* Get screen DC */
	cap->hdc_screen = GetDC(NULL);
	if (!cap->hdc_screen) {
		LOG_ERROR("GetDC failed");
		return false;
	}

	/* Create memory DC */
	cap->hdc_mem = CreateCompatibleDC(cap->hdc_screen);
	if (!cap->hdc_mem) {
		LOG_ERROR("CreateCompatibleDC failed");
		ReleaseDC(NULL, cap->hdc_screen);
		return false;
	}

	/* Create bitmap */
	cap->hbm_screen = CreateCompatibleBitmap(cap->hdc_screen, width, height);
	if (!cap->hbm_screen) {
		LOG_ERROR("CreateCompatibleBitmap failed");
		DeleteDC(cap->hdc_mem);
		ReleaseDC(NULL, cap->hdc_screen);
		return false;
	}

	/* Select bitmap into memory DC */
	SelectObject(cap->hdc_mem, cap->hbm_screen);

	/* Setup BITMAPINFO for DIB */
	memset(&cap->bmi, 0, sizeof(cap->bmi));
	cap->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	cap->bmi.bmiHeader.biWidth = width;
	cap->bmi.bmiHeader.biHeight = -height;  /* Top-down */
	cap->bmi.bmiHeader.biPlanes = 1;
	cap->bmi.bmiHeader.biBitCount = 32;
	cap->bmi.bmiHeader.biCompression = BI_RGB;

	/* Allocate buffer for BGRA data */
	cap->buffer = (uint8_t *)malloc((size_t)width * height * 4);
	if (!cap->buffer) {
		LOG_ERROR("Failed to allocate buffer");
		DeleteObject(cap->hbm_screen);
		DeleteDC(cap->hdc_mem);
		ReleaseDC(NULL, cap->hdc_screen);
		return false;
	}

	cap->initialized = true;
	LOG_INFO("Initialized: %dx%d", width, height);
	return true;
}

void screen_capture_destroy(struct screen_capture *cap)
{
	if (cap->buffer) {
		free(cap->buffer);
		cap->buffer = NULL;
	}
	if (cap->hbm_screen) {
		DeleteObject(cap->hbm_screen);
		cap->hbm_screen = NULL;
	}
	if (cap->hdc_mem) {
		DeleteDC(cap->hdc_mem);
		cap->hdc_mem = NULL;
	}
	if (cap->hdc_screen) {
		ReleaseDC(NULL, cap->hdc_screen);
		cap->hdc_screen = NULL;
	}
	memset(cap, 0, sizeof(*cap));
}

bool screen_capture_grab(struct screen_capture *cap)
{
	if (!cap || !cap->initialized || !cap->hdc_screen || !cap->hdc_mem)
		return false;

	/* Copy screen to memory DC */
	if (!BitBlt(cap->hdc_mem, 0, 0, cap->width, cap->height,
		    cap->hdc_screen, 0, 0, SRCCOPY)) {
		return false;
	}

	/* Get bitmap bits */
	if (!GetDIBits(cap->hdc_mem, cap->hbm_screen, 0, (UINT)cap->height,
		       cap->buffer, &cap->bmi, DIB_RGB_COLORS)) {
		return false;
	}

	return true;
}
