#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define MAX_DETECTIONS 100

// COCO enemy class IDs (FPS games typically detect class 0 = person)
#define COCO_CLASS_PERSON   0
#define COCO_CLASS_CAR      2
#define COCO_CLASS_MOTORCYCLE 3
#define COCO_CLASS_BUS      5
#define COCO_CLASS_TRUCK    7

struct detection {
	float x;      // center x (normalized 0-1)
	float y;      // center y (normalized 0-1)
	float width;  // normalized 0-1
	float height; // normalized 0-1
	float confidence;
	int class_id;
	char class_name[32];
};

struct detection_result {
	struct detection detections[MAX_DETECTIONS];
	int count;
};

struct yolo_inference;

// Create YOLO inference engine from an ONNX model file
struct yolo_inference *yolo_inference_create(const char *model_path, float confidence_threshold, float nms_threshold);

// Destroy inference engine
void yolo_inference_destroy(struct yolo_inference *yi);

// Run inference on an RGBA image.
// Image must be in HWC format (height * width * 4 bytes per pixel).
// Returns true on success.
bool yolo_inference_run(struct yolo_inference *yi, const uint8_t *rgba_data, int width, int height);

// Get the latest detection results
struct detection_result yolo_inference_get_result(struct yolo_inference *yi);

// Update parameters
void yolo_inference_set_confidence_threshold(struct yolo_inference *yi, float threshold);
void yolo_inference_set_nms_threshold(struct yolo_inference *yi, float threshold);

// Check if model is loaded
bool yolo_inference_is_loaded(struct yolo_inference *yi);

// Get model input dimensions
void yolo_inference_get_input_size(struct yolo_inference *yi, int *width, int *height);

// Release the ONNX Runtime DLL loaded globally.
// Call this on plugin unload to avoid resource leaks.
void yolo_inference_shutdown(void);

#ifdef __cplusplus
}
#endif
