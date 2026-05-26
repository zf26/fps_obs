#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aimbot_filter {
	obs_source_t *context;
};

extern struct obs_source_info aimbot_filter_info;

#ifdef __cplusplus
}
#endif
