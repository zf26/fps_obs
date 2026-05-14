cmake_minimum_required(VERSION 3.28...3.30)

# FPS Auto-Aim Bot Plugin for OBS Studio
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

# Enable C++17 for ONNX Runtime
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Windows-specific: DirectX for screen capture
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE dxgi d3d11 dxguid)
endif()

# ONNX Runtime for YOLO inference
option(ENABLE_ENEMY_DETECTION "Enable enemy detection with ONNX Runtime" ON)
if(ENABLE_ENEMY_DETECTION)
  include(cmake/onnxruntime.cmake)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin-main.c
  src/plugin-main.h
  src/screen-capture.h
  src/screen-capture.c
  src/yolo-inference.h
  src/yolo-inference.cpp
  src/aim-control.h
  src/aim-control.c
  src/enemy-detector.h
  src/enemy-detector.c
  src/obs-aimbot-source.h
  src/obs-aimbot-source.c
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
