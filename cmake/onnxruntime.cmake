# ONNX Runtime dependency for FPS aimbot plugin
#
# Downloads ONNX Runtime from GitHub if not found at ONNXRUNTIME_DIR.
# Sets up:
#   onnxruntime_INCLUDE_DIR
#   onnxruntime_LIBRARY

include_guard(GLOBAL)

set(ONNXRUNTIME_VERSION "1.20.1")
set(ONNXRUNTIME_PACKAGE "onnxruntime-win-x64-gpu-${ONNXRUNTIME_VERSION}")

# Default to local installation if exists, otherwise download
if(WIN32)
  set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ONNXRUNTIME_PACKAGE}.zip")
else()
  message(WARNING "FPS aimbot plugin currently only supports Windows")
  return()
endif()

# Allow user to override via -DONNXRUNTIME_DIR=path
# Default to the local GPU package if it exists
if(NOT ONNXRUNTIME_DIR)
  if(EXISTS "D:/onnxruntime-win-x64-gpu-${ONNXRUNTIME_VERSION}")
    set(ONNXRUNTIME_DIR "D:/onnxruntime-win-x64-gpu-${ONNXRUNTIME_VERSION}")
  else()
    set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/onnxruntime")
  endif()
endif()

if(NOT EXISTS "${ONNXRUNTIME_DIR}")
  message(STATUS "ONNX Runtime not found at ${ONNXRUNTIME_DIR}, downloading...")

  set(download_dir "${CMAKE_CURRENT_BINARY_DIR}/onnxruntime-download")
  set(zip_file "${download_dir}/${ONNXRUNTIME_PACKAGE}.zip")

  file(DOWNLOAD "${ONNXRUNTIME_URL}" "${zip_file}"
    STATUS download_status
    SHOW_PROGRESS
  )

  list(GET download_status 0 error_code)
  if(error_code GREATER 0)
    list(GET download_status 1 error_message)
    message(FATAL_ERROR "Failed to download ONNX Runtime: ${error_message}")
  endif()

  file(ARCHIVE_EXTRACT INPUT "${zip_file}" DESTINATION "${download_dir}")

  # Move to final location
  file(RENAME "${download_dir}/${ONNXRUNTIME_PACKAGE}" "${ONNXRUNTIME_DIR}")

  # Cleanup
  file(REMOVE_RECURSE "${download_dir}")

  message(STATUS "ONNX Runtime downloaded to ${ONNXRUNTIME_DIR}")
endif()

set(onnxruntime_INCLUDE_DIR "${ONNXRUNTIME_DIR}/include" CACHE PATH "ONNX Runtime include directory")
set(onnxruntime_LIBRARY "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib" CACHE FILEPATH "ONNX Runtime library")

if(NOT EXISTS "${onnxruntime_INCLUDE_DIR}/onnxruntime_c_api.h")
  message(FATAL_ERROR "ONNX Runtime C API header not found at ${onnxruntime_INCLUDE_DIR}")
endif()

# Dynamic loading only — do NOT link onnxruntime.lib statically (OBS Studio
# links its own CPU-only ONNX Runtime which would override our GPU DLL).
# We load onnxruntime.dll at runtime via LoadLibraryW from the plugin's own
# directory, bypassing OBS's static linking.
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${onnxruntime_INCLUDE_DIR}")

# Copy ONNX Runtime DLLs (including GPU/DML providers) to output and rundir
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_cuda.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_cuda.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_tensorrt.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_tensorrt.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_dml.dll"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_dml.dll"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/"
  COMMENT "Copying ONNX Runtime DLLs (CUDA, TensorRT, DirectML) to build output"
)

message(STATUS "ONNX Runtime: ${onnxruntime_INCLUDE_DIR}")
