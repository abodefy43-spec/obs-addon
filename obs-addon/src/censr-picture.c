/* Censr Studio on OBS: the picture of Censr's own OBS, for Studio's window.
 *
 * The app makes a block of shared memory for it (only this user can open it, and its name has a
 * fresh random part each start) and starts its OBS with CENSR_PICTURE naming it. Each frame of
 * what's live is written in, at the app's preview size, as RGBA; in Studio Mode the scene being
 * set up is drawn on its own, small, once a frame in OBS's own drawing, and written in too (OBS
 * gives a second view's frames only to its encoders). The app takes each new one into its
 * window, so Studio's picture moves as smoothly as OBS's own preview.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "censr-picture.h"

static struct censr_picture_block *block;
static size_t block_size;
/* the picture's size, as checked when the block opened: never read from the block again, so what's
 * written always fits it, whatever its header says later */
static uint32_t width, height;
#ifdef _WIN32
static HANDLE mapping;
#endif
static volatile bool studio_mode;
/* Studio Mode's scene being set up, and where it's drawn (the graphics thread's own) */
static pthread_mutex_t preview_lock;
static obs_source_t *preview_scene;
static gs_texrender_t *drawn;
static gs_stagesurf_t *staged;
static bool waiting; /* last frame's picture is in `staged`, ready to read without a wait */

/* the name the app gave: "censr-picture-" then 32 letters a-f and digits, nothing else */
static bool fair_name(const char *name)
{
	static const char prefix[] = "censr-picture-";
	if (strncmp(name, prefix, sizeof(prefix) - 1) != 0 || strlen(name) != sizeof(prefix) - 1 + 32)
		return false;
	for (const char *c = name + sizeof(prefix) - 1; *c; c++)
		if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f')))
			return false;
	return true;
}

static uint8_t *picture_at(int which)
{
	return (uint8_t *)block + CENSR_PICTURE_HEADER + (size_t)which * width * height * 4;
}

static void write_picture(int which, const struct video_data *frame)
{
	if (!block || !frame->data[0])
		return;
	const size_t row = (size_t)width * 4;
	uint8_t *to = picture_at(which);
	os_atomic_inc_long(&block->seq[which]); /* odd: being written */
	for (uint32_t y = 0; y < height; y++)
		memcpy(to + y * row, frame->data[0] + (size_t)y * frame->linesize[0], row);
	os_atomic_inc_long(&block->seq[which]); /* even: whole */
}

/* each frame of what's live, already made small and RGBA by OBS */
static void on_live(void *param, struct video_data *frame)
{
	UNUSED_PARAMETER(param);
	write_picture(1, frame);
	if (!studio_mode)
		write_picture(0, frame);
}

/* Graphics thread, each frame: last frame's drawing of the scene being set up goes in (read now,
 * a frame after it was asked for, so nothing waits on the graphics card), then this frame's. */
static void draw_preview(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	if (!block || !studio_mode || !drawn || !staged)
		return;
	if (waiting) {
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(staged, &data, &linesize)) {
			struct video_data frame = {.data = {data}, .linesize = {linesize}};
			write_picture(0, &frame);
			gs_stagesurface_unmap(staged);
		}
		waiting = false;
	}
	pthread_mutex_lock(&preview_lock);
	obs_source_t *scene = obs_source_get_ref(preview_scene);
	pthread_mutex_unlock(&preview_lock);
	struct obs_video_info ovi;
	if (!scene || !obs_get_video_info(&ovi)) {
		obs_source_release(scene);
		return;
	}
	gs_texrender_reset(drawn);
	if (gs_texrender_begin(drawn, width, height)) {
		struct vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(scene);
		gs_blend_state_pop();
		gs_texrender_end(drawn);
		gs_stage_texture(staged, gs_texrender_get_texture(drawn));
		waiting = true;
	}
	obs_source_release(scene);
}

static void close_block(void)
{
	if (!block)
		return;
#ifdef _WIN32
	UnmapViewOfFile(block);
	CloseHandle(mapping);
	mapping = NULL;
#else
	munmap(block, block_size);
#endif
	block = NULL;
}

static bool open_block(const char *name)
{
#ifdef _WIN32
	wchar_t wide[64];
	if (_snwprintf(wide, 64, L"Local\\%hs", name) < 0)
		return false;
	mapping = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, wide);
	if (!mapping)
		return false;
	block = MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
	if (!block) {
		CloseHandle(mapping);
		mapping = NULL;
		return false;
	}
	MEMORY_BASIC_INFORMATION info;
	block_size = VirtualQuery(block, &info, sizeof(info)) ? info.RegionSize : 0;
#else
	char path[64];
	snprintf(path, sizeof(path), "/%s", name);
	int fd = shm_open(path, O_RDWR, 0);
	if (fd < 0)
		return false;
	struct stat about;
	if (fstat(fd, &about) != 0 || about.st_uid != getuid() || about.st_size < CENSR_PICTURE_HEADER) {
		close(fd);
		return false;
	}
	block_size = (size_t)about.st_size;
	void *map = mmap(NULL, block_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return false;
	block = map;
#endif
	/* the app's own layout, at a size that fits what it made: the size read once, and kept */
	const uint32_t w = *(volatile uint32_t *)&block->width, h = *(volatile uint32_t *)&block->height;
	const size_t needed = CENSR_PICTURE_HEADER + 2 * (size_t)w * h * 4;
	if (block->magic != CENSR_PICTURE_MAGIC || block->pictures != 2 || !w || !h || w > CENSR_PICTURE_MOST ||
	    h > CENSR_PICTURE_MOST || block_size < needed) {
		close_block();
		return false;
	}
	width = w;
	height = h;
	return true;
}

void picture_start(void)
{
	const char *name = getenv("CENSR_PICTURE");
	if (!name || !getenv("CENSR_STUDIO") || block)
		return;
	pthread_mutex_init(&preview_lock, NULL);
	if (!fair_name(name) || !open_block(name)) {
		blog(LOG_WARNING, "[censr] the app's picture block can't be opened; Studio shows still pictures instead");
		return;
	}
	struct video_scale_info small = {
		.format = VIDEO_FORMAT_RGBA,
		.width = width,
		.height = height,
		.range = VIDEO_RANGE_FULL,
		.colorspace = VIDEO_CS_DEFAULT,
	};
	obs_add_raw_video_callback(&small, on_live, NULL);
	obs_enter_graphics();
	drawn = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	staged = gs_stagesurface_create(width, height, GS_RGBA);
	obs_leave_graphics();
	obs_add_main_render_callback(draw_preview, NULL);
	if (obs_frontend_preview_program_mode_active())
		picture_studio_mode(true);
	blog(LOG_INFO, "[censr] Studio's picture: %ux%u", width, height);
}

/* Studio Mode: the scene being set up, drawn on its own at the picture's size */
void picture_studio_mode(bool on)
{
	if (!block)
		return;
	if (on)
		picture_preview_changed();
	studio_mode = on;
}

void picture_preview_changed(void)
{
	if (!block)
		return;
	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene() : NULL;
	pthread_mutex_lock(&preview_lock);
	obs_source_t *old = preview_scene;
	preview_scene = scene;
	pthread_mutex_unlock(&preview_lock);
	obs_source_release(old);
}

void picture_stop(void)
{
	if (!block)
		return;
	studio_mode = false;
	obs_remove_main_render_callback(draw_preview, NULL);
	obs_remove_raw_video_callback(on_live, NULL);
	obs_enter_graphics();
	gs_texrender_destroy(drawn);
	gs_stagesurface_destroy(staged);
	obs_leave_graphics();
	drawn = NULL;
	staged = NULL;
	pthread_mutex_lock(&preview_lock);
	obs_source_release(preview_scene);
	preview_scene = NULL;
	pthread_mutex_unlock(&preview_lock);
	close_block();
}
