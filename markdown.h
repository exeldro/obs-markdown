#pragma once

#include <obs.h>
#include <util/dstr.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_source_data {
	obs_source_t *source;
	obs_source_t *browser;
	struct dstr html;
	struct dstr markdown_path;
	time_t markdown_time;
	struct dstr css_path;
	time_t css_time;
	pthread_t thread;
	bool stop;
	uint32_t sleep;

	gs_texture_t *texture;
	uint32_t cx;
	uint32_t cy;
};

void render_qt(struct markdown_source_data* md, obs_data_t *settings);

#ifdef __cplusplus
};
#endif
