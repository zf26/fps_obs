# FPS Auto-Aim Bot Plugin for OBS Studio

OBS Studio plugin for real-time enemy detection in FPS games with auto-aim assist. Uses YOLO neural networks via ONNX Runtime + DirectML GPU acceleration.

## Features

- **Game Window Capture** — Captures a specific game window via DXGI (lower latency than full monitor capture)
- **YOLO Real-time Inference** — ONNX Runtime + DirectML GPU acceleration, supports YOLOv5/v7/v8 models
- **Detection Overlay** — Renders enemy bounding boxes and labels in the OBS source
- **Auto-Aim Assist** — Smooth mouse movement to detected enemies with human-like behavior:
  - **Aim Mode**: Center (aim at enemy center), Headshot (aim at top of bounding box), Closest Part
  - **Aim Target**: Nearest enemy, Highest confidence, Closest to crosshair
- **Recoil Control** — Multi-shot recoil compensation with configurable compensation factor
- **Triggerbot** — Optional automatic fire when an enemy enters crosshair
- **Fire Key** — Only aim when a specific key (e.g. mouse button, Shift) is held down
- **Human-like Movement** — Mouse movement uses cubic ease-out curve with micro-jitter
- **Auto-Recovery** — DXGI access lost detection with automatic retry

## Supported Game Types

Works with any FPS game that displays enemy characters visibly on screen. Compatible with YOLO models trained on humanoid/enemy classes (person, enemy soldier, terrorist, etc.).

Recommended YOLO models:
- YOLOv8 trained on FPS game enemies (person class, class 0)
- Custom models targeting specific game characters

## Build Requirements

| Platform | Tool |
|----------|------|
| Windows | Visual Studio 2022 |
| Windows | CMake 3.28+ |
| Windows | OBS Studio 31.1+ (with deps) |

Dependencies (downloaded automatically by CMake):
- ONNX Runtime 1.20.1 (with DirectML)

## Quick Start

### 1. Clone and Configure

```bash
git clone <your-repo>
cd fps-aimbot-plugin
```

### 2. Configure CMake

```bash
cmake -S . -B build_x64 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_DEPS_DIR=/path/to/obs-deps \
  -DOBS_STUDIO_DIR=/path/to/obs-studio
```

CMake automatically downloads ONNX Runtime to the build directory.

### 3. Build

```bash
cmake --build build_x64 --config RelWithDebInfo
```

Build output: `build_x64/RelWithDebInfo/fps-aimbot-plugin.dll`

### 4. Deploy to OBS

Copy `onnxruntime.dll` and `fps-aimbot-plugin.dll` to the OBS plugin directory:

```
D:\obs-studio\obs-plugins\64bit\
  ├── onnxruntime.dll
  └── fps-aimbot-plugin.dll
  └── fps-aimbot-plugin/
      └── locale/
          ├── en-US.ini
          └── zh-CN.ini
```

## In OBS Studio

1. Launch OBS Studio
2. Add **FPS Auto-Aim Bot** source to a scene
3. Configure in properties:
   - **Model Path** — Select your ONNX model file (YOLOv5/v7/v8)
   - **Confidence Threshold** — Detection confidence (default 0.5)
   - **NMS Threshold** — NMS threshold (default 0.45)
   - **Inference Interval** — Inference interval in ms (default 66ms ~ 15 FPS)
   - **Game Window** — Select the game window to capture
   - **Aim Assist** — Enable/disable, aim mode, target mode
   - **Recoil Control** — Enable, compensation strength
   - **Triggerbot** — Enable automatic firing when enemy is in crosshair
   - **Fire Key** — Key that must be held to activate aiming
   - **Show Overlay** — Toggle detection overlay

## Project Structure

```
src/
├── plugin-main.c          # OBS module entry point
├── obs-aimbot-source.c/h  # OBS source plugin implementation
├── screen-capture.c/h     # DXGI screen/window capture
├── yolo-inference.cpp/h   # YOLO ONNX Runtime inference
├── enemy-detector.c/h     # Detection coordinator thread
├── aim-control.c/h       # FPS aim assist and recoil control
cmake/
├── onnxruntime.cmake     # ONNX Runtime dependency management
data/locale/
├── en-US.ini              # English localization
├── zh-CN.ini              # Chinese localization
```

## Technical Highlights

- **DXGI Output Duplication** — Low-latency game window capture via IDXGIOutputDuplication
- **ONNX Runtime DirectML** — GPU-accelerated inference with automatic model introspection
- **Letterbox Preprocessing** — Aspect-ratio-preserving resize with bilinear interpolation
- **NMS (Non-Maximum Suppression)** — Removes overlapping detection boxes
- **QPC Precision Timing** — QueryPerformanceCounter for accurate inference throttling
- **OBS GS Rendering** — gs_vertex2f/gs_render_start for 2D overlay rendering
- **Critical Section Threading** — Thread-safe sharing of detection results between inference and render threads

## License

Based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
