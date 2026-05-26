/*
 * YOLO ONNX inference using ONNX Runtime C API (dynamic loading)
 *
 * ONNX Runtime uses a "GetApi" pattern: the DLL only exports OrtGetApiBase.
 * Call GetApi(ORT_API_VERSION) to get the full OrtApi struct with all function pointers.
 *
 * This bypasses OBS Studio's static linking: we load the GPU DLL from our own
 * directory at runtime and never link against onnxruntime.lib.
 */

#define ORT_NO_DEPRECATED_ANNOTATIONS
#pragma warning(disable : 4996)
#include "yolo-inference.h"
#include "plugin-support.h"

#include <obs-module.h>

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <functional>

#ifdef _MSC_VER
#define strdup _strdup
#define _CRT_NON_CONFORMING_SWPRINTFS 1
#endif

#ifdef _WIN32
#define NOMINMAX
#define _CRT_NON_CONFORMING_SWPRINTFS 1
#include <windows.h>
#endif

/* ONNX Runtime C API — included for types/enums only, no linking */
#include <onnxruntime_c_api.h>

/* ONNX Runtime 1.20.1 uses ORT_API_VERSION 20 */
#define ORT_RUNTIME_API_VERSION 20

typedef const OrtApiBase *(ORT_API_CALL *OrtGetApiBase_t)(void);

/* Global API — populated from OrtGetApiBase */
static const OrtApi *g_ort = NULL;
static HMODULE g_ort_dll = NULL;

/* One OrtEnv per provider — avoids CUDA/DML context conflicts.
 * CUDA and CPU are mutually exclusive on most systems anyway. */
static OrtEnv *g_env_cuda = NULL;
static OrtEnv *g_env_cpu = NULL;

/* Pre-load provider DLLs so onnxruntime.dll can resolve them */
static void preload_provider_dlls(const wchar_t *dir)
{
	const wchar_t *names[] = {
		L"onnxruntime_providers_shared.dll",
		L"onnxruntime_providers_cuda.dll",
	};
	for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
		wchar_t path[512];
		swprintf(path, L"%s\\%s", dir, names[i]);
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
			HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (m) {
				obs_log(LOG_INFO, "Pre-loaded: %s", names[i]);
			}
		}
	}
}

/* Load CUDA / DML provider append functions (exported as DLL symbols) */
static OrtStatus *(ORT_API_CALL *g_OrtSessionOptionsAppendExecutionProvider_CUDA)(
	OrtSessionOptions *, const char *const *) = NULL;
static OrtStatus *(ORT_API_CALL *g_OrtSessionOptionsAppendExecutionProvider_DML)(
	OrtSessionOptions *, const char *const *) = NULL;

static bool init_ort_if_needed(void)
{
	if (g_ort) return true;  /* already loaded */

	wchar_t ort_dll_path[512] = {0};

	/* Priority: 1) hardcoded GPU path, 2) ONNXRUNTIME_DIR env var */
	const wchar_t *candidates[] = {
		L"D:\\onnxruntime-win-x64-gpu-1.25.1\\lib\\onnxruntime.dll",
		L"D:\\onnxruntime-win-x64-gpu-1.20.1\\lib\\onnxruntime.dll",
	};
	for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
		if (GetFileAttributesW(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
			wcscpy_s(ort_dll_path, candidates[i]);
			break;
		}
	}

	if (!ort_dll_path[0]) {
		wchar_t env_dir[512];
		if (GetEnvironmentVariableW(L"ONNXRUNTIME_DIR", env_dir,
					    (DWORD)(sizeof(env_dir) / sizeof(env_dir[0]))) && env_dir[0]) {
			swprintf(ort_dll_path, L"%s\\lib\\onnxruntime.dll", env_dir);
			if (GetFileAttributesW(ort_dll_path) == INVALID_FILE_ATTRIBUTES)
				ort_dll_path[0] = 0;
		}
	}

	if (!ort_dll_path[0]) {
		obs_log(LOG_ERROR,
			"ONNX Runtime GPU DLL not found. "
			"Please install onnxruntime-win-x64-gpu at D:\\ or set ONNXRUNTIME_DIR.");
		return false;
	}

	/* Extract directory for provider pre-loading */
	wchar_t ort_dir[512];
	wcscpy_s(ort_dir, ort_dll_path);
	wchar_t *last_bs = wcsrchr(ort_dir, L'\\');
	if (last_bs) *last_bs = L'\0';
	last_bs = wcsrchr(ort_dir, L'\\');
	if (last_bs) *last_bs = L'\0';  /* remove \lib */

	obs_log(LOG_INFO, "Loading ONNX Runtime from: %S", ort_dll_path);

	preload_provider_dlls(ort_dir);

	/* Load with altered search path so provider DLLs in the same dir are found */
	g_ort_dll = LoadLibraryExW(ort_dll_path, NULL,
		LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	if (!g_ort_dll) {
		obs_log(LOG_ERROR, "LoadLibraryExW failed: %lu", GetLastError());
		return false;
	}

	/* Get OrtGetApiBase — the only exported symbol */
	OrtGetApiBase_t OrtGetApiBase = (OrtGetApiBase_t)
		GetProcAddress(g_ort_dll, "OrtGetApiBase");
	if (!OrtGetApiBase) {
		obs_log(LOG_ERROR, "OrtGetApiBase not found in DLL");
		FreeLibrary(g_ort_dll);
		g_ort_dll = NULL;
		return false;
	}

	/* Get the full OrtApi struct */
	g_ort = OrtGetApiBase()->GetApi(ORT_RUNTIME_API_VERSION);
	if (!g_ort) {
		obs_log(LOG_ERROR, "GetApi(ORT_API_VERSION) returned NULL — version mismatch?");
		FreeLibrary(g_ort_dll);
		g_ort_dll = NULL;
		return false;
	}

	/* Load provider append functions (exported directly from onnxruntime.dll) */
	g_OrtSessionOptionsAppendExecutionProvider_CUDA = (decltype(g_OrtSessionOptionsAppendExecutionProvider_CUDA))
		GetProcAddress(g_ort_dll, "OrtSessionOptionsAppendExecutionProvider_CUDA");
	g_OrtSessionOptionsAppendExecutionProvider_DML = (decltype(g_OrtSessionOptionsAppendExecutionProvider_DML))
		GetProcAddress(g_ort_dll, "OrtSessionOptionsAppendExecutionProvider_DML");

	obs_log(LOG_INFO, "ONNX Runtime loaded successfully");
	return true;
}

/* Get ORT env for a specific provider. CUDA/DML use their own envs. */
static OrtEnv *get_ort_env_for_provider(const char *provider)
{
	if (!g_ort) return NULL;

	if (strcmp(provider, "CUDA") == 0 || strcmp(provider, "DirectML") == 0) {
		if (g_env_cuda) return g_env_cuda;
		OrtStatus *st = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "fps-aimbot-gpu", &g_env_cuda);
		if (st || !g_env_cuda) {
			obs_log(LOG_ERROR, "Failed to create ONNX Runtime GPU env");
			if (st) {
				const char *msg = g_ort->GetErrorMessage(st);
				if (msg) obs_log(LOG_ERROR, "ORT error: %s", msg);
				g_ort->ReleaseStatus(st);
			}
			g_env_cuda = NULL;
			return NULL;
		}
		return g_env_cuda;
	}

	/* CPU fallback */
	if (g_env_cpu) return g_env_cpu;
	OrtStatus *st = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "fps-aimbot-cpu", &g_env_cpu);
	if (st) {
		obs_log(LOG_ERROR, "Failed to create ONNX Runtime CPU env");
		const char *msg = g_ort->GetErrorMessage(st);
		if (msg) obs_log(LOG_ERROR, "ORT error: %s", msg);
		g_ort->ReleaseStatus(st);
		g_env_cpu = NULL;
	}
	return g_env_cpu;
}

static void shutdown_ort(void)
{
	if (g_env_cuda) { g_ort->ReleaseEnv(g_env_cuda); g_env_cuda = NULL; }
	if (g_env_cpu)  { g_ort->ReleaseEnv(g_env_cpu);  g_env_cpu = NULL; }
	if (g_ort_dll) {
		FreeLibrary(g_ort_dll);
		g_ort_dll = NULL;
		g_ort = NULL;
	}
}

void yolo_inference_shutdown(void)
{
	shutdown_ort();
}

extern "C" {

struct yolo_inference {
	OrtSession *session;

	char *model_path;
	char *input_name;
	char *output_name;
	float confidence_threshold;
	float nms_threshold;

	int input_width;
	int input_height;

	std::vector<float> input_buffer;
	std::vector<float> box_buffer;
	std::vector<float> score_buffer;
	std::vector<int> class_buffer;
	std::vector<float> norm_box_buffer;

	struct detection_result last_result;
	bool loaded;
};

static float clamp_float(float val, float lo, float hi)
{
	return val < lo ? lo : val > hi ? hi : val;
}

static float sigmoid(float x)
{
	return 1.0f / (1.0f + std::exp(-x));
}

struct yolo_inference *yolo_inference_create(const char *model_path,
	float confidence_threshold, float nms_threshold)
{
	if (!init_ort_if_needed())
		return nullptr;

	auto *yi = new yolo_inference();
	if (!yi) return nullptr;

	yi->model_path = strdup(model_path);
	yi->confidence_threshold = confidence_threshold;
	yi->nms_threshold = nms_threshold;
	yi->last_result.count = 0;
	yi->session = nullptr;
	yi->loaded = false;

	const OrtApi *ort = g_ort;

#ifdef _WIN32
	int wlen = MultiByteToWideChar(CP_UTF8, 0, model_path, -1, NULL, 0);
	std::wstring wpath((size_t)wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, model_path, -1, &wpath[0], wlen);
	wpath.resize((size_t)wlen - 1);
	const ORTCHAR_T *model_path_w = wpath.c_str();
#endif

	/* Try providers in order: CUDA -> DirectML -> CPU.
	 * GPU providers are tried first for best inference performance.
	 * If a GPU provider fails (DLL missing, no compatible GPU), falls back to CPU. */
	OrtSession *sess = nullptr;
	const char *provider_name = nullptr;

	/* CUDA — fastest option on NVIDIA GPUs */
	if (!sess) {
		OrtSessionOptions *opts = nullptr;
		OrtStatus *st = ort->CreateSessionOptions(&opts);
		if (!st && opts) {
			ort->SetIntraOpNumThreads(opts, 0);
			ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_EXTENDED);
			if (g_OrtSessionOptionsAppendExecutionProvider_CUDA) {
				st = g_OrtSessionOptionsAppendExecutionProvider_CUDA(opts, NULL);
			} else {
				st = ort->CreateStatus(ORT_INVALID_ARGUMENT, "CUDA provider not available");
			}
			if (!st) {
				OrtEnv *env = get_ort_env_for_provider("CUDA");
				if (env) {
					st = ort->CreateSession(env, model_path_w, opts, &sess);
				} else {
					st = ort->CreateStatus(ORT_INVALID_ARGUMENT, "CUDA env unavailable");
				}
			}
			if (!st && sess) {
				provider_name = "CUDA";
				obs_log(LOG_INFO, "Using CUDA execution provider");
			} else if (st) {
				const char *err = ort->GetErrorMessage(st);
				obs_log(LOG_WARNING, "CUDA provider init failed: %s",
					err ? err : "unknown");
				ort->ReleaseStatus(st);
			}
			ort->ReleaseSessionOptions(opts);
		}
	}

	/* DirectML — Windows-native GPU acceleration (works on AMD/Intel/NVIDIA) */
	if (!sess) {
		OrtSessionOptions *opts = nullptr;
		OrtStatus *st = ort->CreateSessionOptions(&opts);
		if (!st && opts) {
			ort->SetIntraOpNumThreads(opts, 0);
			ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_EXTENDED);
			if (g_OrtSessionOptionsAppendExecutionProvider_DML) {
				st = g_OrtSessionOptionsAppendExecutionProvider_DML(opts, NULL);
			} else {
				st = ort->CreateStatus(ORT_INVALID_ARGUMENT, "DML provider not available");
			}
			if (!st) {
				OrtEnv *env = get_ort_env_for_provider("DirectML");
				if (env) {
					st = ort->CreateSession(env, model_path_w, opts, &sess);
				} else {
					st = ort->CreateStatus(ORT_INVALID_ARGUMENT, "DML env unavailable");
				}
			}
			if (!st && sess) {
				provider_name = "DirectML";
				obs_log(LOG_INFO, "Using DirectML execution provider");
			} else if (st) {
				const char *err = ort->GetErrorMessage(st);
				obs_log(LOG_WARNING, "DirectML provider init failed: %s",
					err ? err : "unknown");
				ort->ReleaseStatus(st);
			}
			ort->ReleaseSessionOptions(opts);
		}
	}

	/* CPU fallback — always works, fast enough for 256x256 YOLO at 30fps */
	if (!sess) {
		OrtSessionOptions *opts = nullptr;
		OrtStatus *st = ort->CreateSessionOptions(&opts);
		if (!st && opts) {
			ort->SetIntraOpNumThreads(opts, 6);
			ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_EXTENDED);
			st = ort->CreateSession(get_ort_env_for_provider("CPU"), model_path_w, opts, &sess);
			ort->ReleaseSessionOptions(opts);
			if (!st && sess) {
				provider_name = "CPU";
				obs_log(LOG_INFO, "Using CPU execution provider (6 threads)");
			} else {
				obs_log(LOG_ERROR, "All inference providers failed");
				if (st) {
					const char *err = ort->GetErrorMessage(st);
					obs_log(LOG_ERROR, "Last ORT error: %s", err ? err : "unknown");
					ort->ReleaseStatus(st);
				}
				delete yi;
				return nullptr;
			}
		}
	}

	if (!sess) {
		delete yi;
		return nullptr;
	}
	yi->session = sess;

	/* Get input/output names */
	{
		OrtAllocator *alloc = nullptr;
		ort->GetAllocatorWithDefaultOptions(&alloc);
		if (alloc) {
			char *in_name = nullptr;
			OrtStatus *st = ort->SessionGetInputName(sess, 0, alloc, &in_name);
			if (!st && in_name) yi->input_name = strdup(in_name);
			ort->AllocatorFree(alloc, in_name);

			char *out_name = nullptr;
			st = ort->SessionGetOutputName(sess, 0, alloc, &out_name);
			if (!st && out_name) yi->output_name = strdup(out_name);
			ort->AllocatorFree(alloc, out_name);
		}
		if (!yi->input_name) yi->input_name = strdup("images");
		if (!yi->output_name) yi->output_name = strdup("output0");
	}

	/* Get input shape */
	{
		OrtTypeInfo *type_info = nullptr;
		OrtStatus *st = ort->SessionGetInputTypeInfo(sess, 0, &type_info);
		if (!st && type_info) {
			const OrtTensorTypeAndShapeInfo *shape_info = nullptr;
			if (!ort->CastTypeInfoToTensorInfo(type_info, &shape_info) && shape_info) {
				size_t dim_count = 0;
				ort->GetDimensionsCount(shape_info, &dim_count);
				if (dim_count > 0) {
					std::vector<int64_t> shape(dim_count);
					ort->GetDimensions(shape_info, shape.data(), dim_count);
					if (dim_count >= 4) {
						yi->input_height = (int)shape[2];
						yi->input_width  = (int)shape[3];
					}
				}
			}
			ort->ReleaseTypeInfo(type_info);
		}
		if (yi->input_width <= 0)  yi->input_width  = 640;
		if (yi->input_height <= 0) yi->input_height = 640;
	}

	yi->loaded = true;
	obs_log(LOG_INFO, "YOLO model loaded: %s (input: %dx%d, tensor: %s / %s)",
		model_path, yi->input_width, yi->input_height,
		yi->input_name, yi->output_name);

	return yi;
}

void yolo_inference_destroy(struct yolo_inference *yi)
{
	if (!yi) return;
	free(yi->model_path);
	free(yi->input_name);
	free(yi->output_name);
	if (yi->session) g_ort->ReleaseSession(yi->session);
	delete yi;
}

static void letterbox(const uint8_t *src, int src_w, int src_h,
	float *dst, int dst_w, int dst_h)
{
	float scale = std::min((float)dst_w / src_w, (float)dst_h / src_h);
	int new_w = (int)(src_w * scale);
	int new_h = (int)(src_h * scale);
	int pad_x = (dst_w - new_w) / 2;
	int pad_y = (dst_h - new_h) / 2;

	for (int i = 0; i < dst_w * dst_h * 3; i++)
		dst[i] = 114.0f / 255.0f;

	for (int y = 0; y < new_h; y++) {
		float src_y = (float)y / new_h * src_h;
		int sy0 = (int)src_y;
		int sy1 = std::min(sy0 + 1, src_h - 1);
		float fy = src_y - sy0;

		for (int x = 0; x < new_w; x++) {
			float src_x = (float)x / new_w * src_w;
			int sx0 = (int)src_x;
			int sx1 = std::min(sx0 + 1, src_w - 1);
			float fx = src_x - sx0;

			int dst_idx = (pad_y + y) * dst_w + (pad_x + x);

			for (int c = 0; c < 3; c++) {
				float v00 = src[(sy0 * src_w + sx0) * 4 + c];
				float v10 = src[(sy0 * src_w + sx1) * 4 + c];
				float v01 = src[(sy1 * src_w + sx0) * 4 + c];
				float v11 = src[(sy1 * src_w + sx1) * 4 + c];

				float v0 = v00 * (1 - fx) + v10 * fx;
				float v1 = v01 * (1 - fy) + v11 * fy;
				dst[c * dst_w * dst_h + dst_idx] =
					(v0 * (1 - fy) + v1 * fy) / 255.0f;
			}
		}
	}
}

static void nms(const std::vector<float> &boxes,
	const std::vector<float> &scores,
	const std::vector<int> &indices,
	float iou_threshold, std::vector<int> &result)
{
	result.clear();
	int n = (int)indices.size();
	std::vector<bool> suppressed(n, false);

	for (int i = 0; i < n; i++) {
		if (suppressed[i]) continue;
		int idx_i = indices[i];
		result.push_back(idx_i);

		float x1_i = boxes[idx_i * 4];
		float y1_i = boxes[idx_i * 4 + 1];
		float x2_i = boxes[idx_i * 4 + 2];
		float y2_i = boxes[idx_i * 4 + 3];
		float area_i = (x2_i - x1_i) * (y2_i - y1_i);

		for (int j = i + 1; j < n; j++) {
			if (suppressed[j]) continue;
			int idx_j = indices[j];

			float ix1 = std::max(x1_i, boxes[idx_j * 4]);
			float iy1 = std::max(y1_i, boxes[idx_j * 4 + 1]);
			float ix2 = std::min(x2_i, boxes[idx_j * 4 + 2]);
			float iy2 = std::min(y2_i, boxes[idx_j * 4 + 3]);

			float iw = std::max(0.0f, ix2 - ix1);
			float ih = std::max(0.0f, iy2 - iy1);
			float inter = iw * ih;

			float area_j = (boxes[idx_j * 4 + 2] - boxes[idx_j * 4]) *
				       (boxes[idx_j * 4 + 3] - boxes[idx_j * 4 + 1]);
			float iou = inter / (area_i + area_j - inter);

			if (iou > iou_threshold) suppressed[j] = true;
		}
	}
}

static const char *coco_class_name(int class_id)
{
	switch (class_id) {
	case 0:  return "enemy";
	case 2:  return "car";
	case 3:  return "motorcycle";
	case 5:  return "bus";
	case 7:  return "truck";
	default: return "target";
	}
}

static bool is_enemy_class(int class_id)
{
	return class_id == 0 || class_id == 2 || class_id == 3 || class_id == 5 || class_id == 7;
}

bool yolo_inference_run(struct yolo_inference *yi,
	const uint8_t *rgba_data, int width, int height)
{
	if (!yi || !yi->loaded || !rgba_data || !yi->session)
		return false;

	const OrtApi *ort = g_ort;

	/* Pre-process */
	yi->input_buffer.resize((size_t)yi->input_width * yi->input_height * 3);
	letterbox(rgba_data, width, height,
		yi->input_buffer.data(),
		yi->input_width, yi->input_height);

	/* Create input tensor */
	OrtMemoryInfo *mem_info = nullptr;
	ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
	if (!mem_info) return false;

	int64_t input_shape[] = {1, 3, yi->input_height, yi->input_width};
	OrtValue *input_tensor = nullptr;

	OrtStatus *st = ort->CreateTensorWithDataAsOrtValue(mem_info,
		yi->input_buffer.data(),
		yi->input_buffer.size() * sizeof(float),
		input_shape, 4,
		ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
		&input_tensor);

	ort->ReleaseMemoryInfo(mem_info);

	if (st || !input_tensor) {
		obs_log(LOG_ERROR, "Failed to create input tensor");
		return false;
	}

	/* Run inference */
	const char *input_names[] = {yi->input_name ? yi->input_name : "images"};
	const char *output_names[] = {yi->output_name ? yi->output_name : "output0"};
	OrtValue *output_tensor = nullptr;

	st = ort->Run(yi->session, nullptr,
		input_names, (const OrtValue *const *)&input_tensor, 1,
		output_names, 1,
		&output_tensor);

	ort->ReleaseValue(input_tensor);

	if (st || !output_tensor) {
		obs_log(LOG_ERROR, "ONNX Run failed");
		return false;
	}

	/* Verify output is a tensor */
	int is_tensor = 0;
	ort->IsTensor(output_tensor, &is_tensor);
	if (!is_tensor) {
		obs_log(LOG_ERROR, "Output is not a tensor");
		ort->ReleaseValue(output_tensor);
		return false;
	}

	/* Get output data pointer */
	void *output_data = nullptr;
	ort->GetTensorMutableData(output_tensor, &output_data);

	/* Get output shape */
	size_t elem_count = 0;
	OrtTensorTypeAndShapeInfo *shape_info = nullptr;
	ort->GetTensorTypeAndShape(output_tensor, &shape_info);

	int num_detections = 0;
	int num_channels = 0;
	bool is_yolov8 = false;

	if (shape_info) {
		size_t count = 0;
		ort->GetDimensionsCount(shape_info, &count);
		if (count >= 3) {
			int64_t dims[8] = {0};
			ort->GetDimensions(shape_info, dims, (size_t)8);
			int64_t d1 = dims[1], d2 = dims[2];
			/* YOLOv8: [1, 84, 8400] — d1 >= 84, d1 < 300, d2 > 1000 */
			if (d1 >= 84 && d1 < 300 && d2 > 1000) {
				num_detections = (int)d2;
				num_channels   = (int)d1;
				is_yolov8 = true;
			} else if (d2 >= 85) {
				/* YOLOv5: [1, N, 85] */
				num_detections = (int)d2;
				num_channels   = (int)d1;
			}
		}
		ort->ReleaseTensorTypeAndShapeInfo(shape_info);
	}

	if (num_detections == 0) {
		obs_log(LOG_WARNING, "Could not determine output format");
		ort->ReleaseValue(output_tensor);
		return false;
	}

	float *data = (float *)output_data;

	yi->box_buffer.clear();
	yi->score_buffer.clear();
	yi->class_buffer.clear();
	int detection_count = 0;

	float scale = std::min((float)yi->input_width / width,
		(float)yi->input_height / height);
	int new_w = (int)(width * scale);
	int new_h = (int)(height * scale);
	int pad_x = (yi->input_width - new_w) / 2;
	int pad_y = (yi->input_height - new_h) / 2;

	for (int i = 0; i < num_detections; i++) {
		float confidence = 0.0f;
		int class_id = -1;

		if (is_yolov8) {
			float *class_data = &data[4 * num_detections + i];
			float max_score = 0.0f;
			int max_class = -1;
			for (int c = 0; c < 80; c++) {
				float s = class_data[c * num_detections];
				if (s > max_score) {
					max_score = s;
					max_class = c;
				}
			}
			confidence = max_score;
			class_id = max_class;
		} else {
			int off = i * num_channels;
			float obj = sigmoid(data[off + 4]);
			float max_score = 0.0f;
			int max_class = -1;
			for (int c = 0; c < 80; c++) {
				float s = sigmoid(data[off + 5 + c]);
				if (s * obj > max_score) {
					max_score = s * obj;
					max_class = c;
				}
			}
			confidence = max_score;
			class_id = max_class;
		}

		if (class_id < 0 || confidence < yi->confidence_threshold) continue;
		if (!is_enemy_class(class_id)) continue;

		float xc, yc, w, h;
		if (is_yolov8) {
			xc = data[i];
			yc = data[1 * num_detections + i];
			w  = data[2 * num_detections + i];
			h  = data[3 * num_detections + i];
		} else {
			int off = i * num_channels;
			xc = data[off];
			yc = data[off + 1];
			w  = data[off + 2];
			h  = data[off + 3];
		}

		float x1 = clamp_float(((xc - w / 2) - pad_x) / scale, 0, (float)width);
		float y1 = clamp_float(((yc - h / 2) - pad_y) / scale, 0, (float)height);
		float x2 = clamp_float(((xc + w / 2) - pad_x) / scale, 0, (float)width);
		float y2 = clamp_float(((yc + h / 2) - pad_y) / scale, 0, (float)height);

		yi->box_buffer.push_back(x1);
		yi->box_buffer.push_back(y1);
		yi->box_buffer.push_back(x2);
		yi->box_buffer.push_back(y2);
		yi->score_buffer.push_back(confidence);
		yi->class_buffer.push_back(class_id);
		detection_count++;
	}

	ort->ReleaseValue(output_tensor);
	yi->last_result.count = 0;

	if (detection_count > 0) {
		obs_log(LOG_INFO, "[YOLO] Raw detections before NMS: %d", detection_count);
		for (int i = 0; i < detection_count; i++) {
			obs_log(LOG_INFO, "  [raw %d] class=%d (%s), conf=%.3f, box=[%.1f, %.1f, %.1f, %.1f]",
				i, yi->class_buffer[i], coco_class_name(yi->class_buffer[i]),
				yi->score_buffer[i],
				yi->box_buffer[i * 4], yi->box_buffer[i * 4 + 1],
				yi->box_buffer[i * 4 + 2], yi->box_buffer[i * 4 + 3]);
		}

		std::vector<int> sorted(detection_count);
		for (int i = 0; i < detection_count; i++) sorted[i] = i;
		std::sort(sorted.begin(), sorted.end(),
			[&](int a, int b) {
				return yi->score_buffer[a] > yi->score_buffer[b];
			});

		yi->norm_box_buffer.clear();
		std::vector<int> nms_idx;
		for (int i = 0; i < detection_count; i++) {
			int idx = sorted[i];
			yi->norm_box_buffer.push_back(yi->box_buffer[idx * 4]     / width);
			yi->norm_box_buffer.push_back(yi->box_buffer[idx * 4 + 1] / height);
			yi->norm_box_buffer.push_back(yi->box_buffer[idx * 4 + 2] / width);
			yi->norm_box_buffer.push_back(yi->box_buffer[idx * 4 + 3] / height);
			nms_idx.push_back(i);
		}

		std::vector<int> keep;
		nms(yi->norm_box_buffer, yi->score_buffer, nms_idx,
			yi->nms_threshold, keep);

		for (int ki : keep) {
			if (yi->last_result.count >= MAX_DETECTIONS) break;
			int orig = sorted[ki];
			auto &det = yi->last_result
					.detections[yi->last_result.count];

			float x1 = yi->box_buffer[orig * 4];
			float y1 = yi->box_buffer[orig * 4 + 1];
			float x2 = yi->box_buffer[orig * 4 + 2];
			float y2 = yi->box_buffer[orig * 4 + 3];

			det.x = ((x1 + x2) / 2) / width;
			det.y = ((y1 + y2) / 2) / height;
			det.width  = (x2 - x1) / width;
			det.height = (y2 - y1) / height;
			det.confidence = yi->score_buffer[orig];
			det.class_id = yi->class_buffer[orig];
			strncpy(det.class_name,
				coco_class_name(det.class_id),
				sizeof(det.class_name) - 1);
			det.class_name[sizeof(det.class_name) - 1] = '\0';
			yi->last_result.count++;
		}

		obs_log(LOG_INFO, "[YOLO] Final detections after NMS: %d", yi->last_result.count);
		for (int i = 0; i < yi->last_result.count; i++) {
			auto &d = yi->last_result.detections[i];
			obs_log(LOG_INFO, "  [det %d] class=%d (%s), conf=%.3f, center=[%.3f, %.3f], size=[%.3f, %.3f]",
				i, d.class_id, d.class_name, d.confidence,
				d.x, d.y, d.width, d.height);
		}
	}

	return true;
}

struct detection_result yolo_inference_get_result(struct yolo_inference *yi)
{
	if (!yi) return {};
	return yi->last_result;
}

void yolo_inference_set_confidence_threshold(struct yolo_inference *yi, float threshold)
{
	if (yi) yi->confidence_threshold = threshold;
}

void yolo_inference_set_nms_threshold(struct yolo_inference *yi, float threshold)
{
	if (yi) yi->nms_threshold = threshold;
}

bool yolo_inference_is_loaded(struct yolo_inference *yi)
{
	return yi && yi->loaded;
}

void yolo_inference_get_input_size(struct yolo_inference *yi, int *width, int *height)
{
	if (yi) {
		*width = yi->input_width;
		*height = yi->input_height;
	}
}

} // extern "C"
