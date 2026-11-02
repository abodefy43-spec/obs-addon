/* Censr for OBS: the add-on inside OBS.
 *
 * It plays Censr's cleaned sound in OBS (a "Censr" sound of its own), and when Censr says so it
 * takes over OBS's own mic, and the computer's sound when asked: it mutes them, writing down
 * which ones it muted, and plays Censr's in their place. Giving back unmutes exactly those. If
 * Censr goes away mid-stream, OBS stays silent rather than send the raw mic, until Censr is back
 * or the person chooses Tools > "Censr: give OBS its own sound back". Sound it muted is given
 * back when OBS next starts, should OBS close while it's taken over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>
#include <util/darray.h>

#include "censr-link.h"
#include "censr-picture.h"
#include "censr-studio.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("censr-obs", "en-US")

/* the output channel Censr's sound plays on: past OBS's own six for desktop audio and mics */
#define CHANNEL 63
/* the name of OBS's own video delay as Censr puts it on a webcam */
#define DELAY_FILTER "Censr: matches your voice"

static struct censr_link *link;
static obs_source_t *censr_sound;
static pthread_mutex_t lock;
static uint32_t rate = 48000, channels = 2;
static bool taken;
static obs_data_array_t *muted_by_us;   /* the uuids of the sources Censr muted */
static obs_data_array_t *delayed_by_us; /* the uuids of the webcams Censr held back */

/* captions waiting for their sound to play: a stream caption shows about 32 characters, so a
 * sentence goes as pieces, one after another (UTF-8, each character up to 4 bytes) */
#define CAPTION_CHARS 32
struct caption_piece {
	uint64_t due;
	double seconds;
	char text[CAPTION_CHARS * 4 + 1];
};
static DARRAY(struct caption_piece) captions;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Censr for OBS: Censr's cleaned sound in place of your own mic.";
}

/* ---- the sounds Censr takes over ---- */

static bool is_mic(const char *id)
{
	static const char *mics[] = {"pulse_input_capture", "alsa_input_capture", "wasapi_input_capture",
				     "coreaudio_input_capture", "jack_input_client", NULL};
	for (const char **m = mics; *m; m++)
		if (strcmp(id, *m) == 0)
			return true;
	return false;
}

static bool is_webcam(const char *id)
{
	static const char *cams[] = {"v4l2_input", "dshow_input", "av_capture_input", "av_capture_input_v2",
				     "macos-avcapture", "macos-avcapture-fast", NULL};
	for (const char **c = cams; *c; c++)
		if (strcmp(id, *c) == 0)
			return true;
	return false;
}

static bool is_computer(const char *id)
{
	static const char *outs[] = {"pulse_output_capture", "wasapi_output_capture", "wasapi_process_output_capture",
				     "coreaudio_output_capture", "sck_audio_capture", NULL};
	for (const char **o = outs; *o; o++)
		if (strcmp(id, *o) == 0)
			return true;
	return false;
}

static char *taken_file(void)
{
	return obs_module_config_path("taken.json");
}

/* what Censr muted, kept on disk so it's given back even after OBS closes while taken over */
static void save_taken(void)
{
	char *path = taken_file();
	if (!path)
		return;
	if (obs_data_array_count(muted_by_us) == 0 && obs_data_array_count(delayed_by_us) == 0) {
		os_unlink(path);
	} else {
		char *folder = obs_module_config_path("");
		os_mkdirs(folder);
		bfree(folder);
		obs_data_t *data = obs_data_create();
		obs_data_set_array(data, "muted", muted_by_us);
		obs_data_set_array(data, "delayed", delayed_by_us);
		obs_data_save_json_safe(data, path, "tmp", "bak");
		obs_data_release(data);
	}
	bfree(path);
}

struct take_task {
	bool computer;
	int delay_ms;
};

static void remember(obs_data_array_t *list, obs_source_t *source)
{
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "uuid", obs_source_get_uuid(source));
	obs_data_array_push_back(list, item);
	obs_data_release(item);
}

/* OBS's own video delay on a webcam, as long as the voice waits; set again if it's there */
static void delay_webcam(obs_source_t *source, int delay_ms)
{
	obs_source_t *filter = obs_source_get_filter_by_name(source, DELAY_FILTER);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "delay_ms", delay_ms);
	if (filter) {
		obs_source_update(filter, settings);
		obs_source_release(filter);
	} else {
		filter = obs_source_create("async_delay_filter", DELAY_FILTER, settings, NULL);
		if (filter) {
			obs_source_filter_add(source, filter);
			obs_source_release(filter);
			remember(delayed_by_us, source);
		}
	}
	obs_data_release(settings);
}

static bool mute_one(void *param, obs_source_t *source)
{
	const struct take_task *task = param;
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || source == censr_sound)
		return true;
	if (is_webcam(id) && task->delay_ms > 0) {
		delay_webcam(source, task->delay_ms);
		return true;
	}
	if (!is_mic(id) && !(task->computer && is_computer(id)))
		return true;
	if (obs_source_muted(source))
		return true; /* muted by the person: theirs to unmute, never ours */
	obs_source_set_muted(source, true);
	remember(muted_by_us, source);
	return true;
}

static void take_now(void *param)
{
	struct take_task *task = param;
	pthread_mutex_lock(&lock);
	obs_enum_sources(mute_one, task);
	save_taken();
	if (!taken)
		obs_set_output_source(CHANNEL, censr_sound);
	taken = true;
	pthread_mutex_unlock(&lock);
	censr_link_say(link, true);
	bfree(task);
}

/* Unmutes what Censr muted. [remember] keeps the note on disk, so OBS's next start unmutes them
 * again: at closing, OBS may already have saved them muted. */
static void unmute_ours(bool remember)
{
	pthread_mutex_lock(&lock);
	size_t count = obs_data_array_count(muted_by_us);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(muted_by_us, i);
		obs_source_t *source = obs_get_source_by_uuid(obs_data_get_string(item, "uuid"));
		if (source) {
			obs_source_set_muted(source, false);
			obs_source_release(source);
		}
		obs_data_release(item);
	}
	/* webcams play on time again */
	count = obs_data_array_count(delayed_by_us);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(delayed_by_us, i);
		obs_source_t *source = obs_get_source_by_uuid(obs_data_get_string(item, "uuid"));
		if (source) {
			obs_source_t *filter = obs_source_get_filter_by_name(source, DELAY_FILTER);
			if (filter) {
				obs_source_filter_remove(source, filter);
				obs_source_release(filter);
			}
			obs_source_release(source);
		}
		obs_data_release(item);
	}
	if (!remember) {
		obs_data_array_release(muted_by_us);
		muted_by_us = obs_data_array_create();
		obs_data_array_release(delayed_by_us);
		delayed_by_us = obs_data_array_create();
		save_taken();
	}
	if (taken)
		obs_set_output_source(CHANNEL, NULL);
	taken = false;
	pthread_mutex_unlock(&lock);
	censr_link_say(link, false);
}

static void give_back_now(void *param)
{
	UNUSED_PARAMETER(param);
	unmute_ours(false);
}

/* sound muted before OBS last closed, given back now (Censr asks again if it still wants it) */
static void give_back_from_last_time(void)
{
	char *path = taken_file();
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	if (!data)
		return;
	pthread_mutex_lock(&lock);
	obs_data_array_t *saved = obs_data_get_array(data, "muted");
	if (saved) {
		obs_data_array_release(muted_by_us);
		muted_by_us = saved;
	}
	obs_data_array_t *held = obs_data_get_array(data, "delayed");
	if (held) {
		obs_data_array_release(delayed_by_us);
		delayed_by_us = held;
	}
	pthread_mutex_unlock(&lock);
	obs_data_release(data);
	give_back_now(NULL);
}

/* ---- the link's callbacks (on the link's own thread) ---- */

static void on_shape(uint32_t r, uint32_t c, void *data)
{
	UNUSED_PARAMETER(data);
	pthread_mutex_lock(&lock);
	rate = r;
	channels = c;
	pthread_mutex_unlock(&lock);
}

static void on_audio(const uint8_t *samples, size_t bytes, void *data)
{
	UNUSED_PARAMETER(data);
	pthread_mutex_lock(&lock);
	uint32_t r = rate, c = channels;
	pthread_mutex_unlock(&lock);
	struct obs_source_audio audio = {0};
	audio.data[0] = samples;
	audio.frames = (uint32_t)(bytes / (2 * c));
	audio.speakers = c == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_16BIT;
	audio.samples_per_sec = r;
	audio.timestamp = os_gettime_ns();
	if (audio.frames)
		obs_source_output_audio(censr_sound, &audio);
}

static void on_take(bool computer, int delay_ms, void *data)
{
	UNUSED_PARAMETER(data);
	struct take_task *task = bzalloc(sizeof(*task));
	task->computer = computer;
	task->delay_ms = delay_ms;
	obs_queue_task(OBS_TASK_UI, take_now, task, false);
}

static void on_give_back(void *data)
{
	UNUSED_PARAMETER(data);
	obs_queue_task(OBS_TASK_UI, give_back_now, NULL, false);
}

/* a UTF-8 character's later bytes, which a piece never starts or ends at */
static bool continues(char c)
{
	return ((unsigned char)c & 0xC0) == 0x80;
}

static void on_caption(const char *text, int after_ms, void *data)
{
	UNUSED_PARAMETER(data);
	uint64_t due = os_gettime_ns() + (uint64_t)after_ms * 1000000ULL;
	size_t length = strlen(text);
	pthread_mutex_lock(&lock);
	for (size_t at = 0; at < length;) {
		while (at < length && text[at] == ' ')
			at++;
		/* up to CAPTION_CHARS whole characters */
		size_t take = 0, chars = 0;
		while (at + take < length && chars < CAPTION_CHARS) {
			take++;
			while (at + take < length && continues(text[at + take]))
				take++;
			chars++;
		}
		if (at + take < length) /* break between words when there's a space to break at */
			for (size_t back = take; back > take / 2; back--)
				if (text[at + back] == ' ') {
					take = back;
					break;
				}
		if (!take)
			break;
		chars = 0;
		for (size_t i = 0; i < take; i++)
			chars += !continues(text[at + i]);
		struct caption_piece *piece = da_push_back_new(captions);
		memcpy(piece->text, text + at, take);
		piece->text[take] = 0;
		piece->seconds = chars / 14.0 < 1.5 ? 1.5 : chars / 14.0; /* about reading speed */
		piece->due = due;
		due += (uint64_t)(piece->seconds * 1e9);
		at += take;
	}
	pthread_mutex_unlock(&lock);
}

/* each frame: the captions whose time has come go into the stream, if there's a stream */
static void caption_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(seconds);
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&lock);
	while (captions.num && captions.array[0].due <= now) {
		struct caption_piece piece = captions.array[0];
		da_erase(captions, 0);
		pthread_mutex_unlock(&lock);
		obs_output_t *stream = obs_frontend_get_streaming_output();
		if (stream) {
			if (obs_output_active(stream))
				obs_output_output_caption_text2(stream, piece.text, piece.seconds);
			obs_output_release(stream);
		}
		pthread_mutex_lock(&lock);
	}
	pthread_mutex_unlock(&lock);
}

static void on_connected(bool on, void *data)
{
	UNUSED_PARAMETER(data);
	/* Censr gone mid-stream: OBS's own sound stays muted, so nothing un-checked goes out */
	blog(LOG_INFO, "[censr] %s Censr", on ? "linked to" : "lost");
}

/* ---- the "Censr" sound ---- */

static const char *censr_sound_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CensrSound");
}

static void *censr_sound_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	return source;
}

static void censr_sound_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static struct obs_source_info censr_sound_info = {
	.id = "censr_sound",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* made by the add-on itself, never from OBS's Add menu */
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CAP_DISABLED,
	.get_name = censr_sound_name,
	.create = censr_sound_create,
	.destroy = censr_sound_destroy,
};

/* ---- OBS's menu and its moments ---- */

static void give_back_menu(void *data)
{
	UNUSED_PARAMETER(data);
	give_back_now(NULL);
}

static void on_frontend(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		picture_start();
		give_back_from_last_time();
		link = censr_link_start(&(struct censr_link_events){
			.shape = on_shape,
			.audio = on_audio,
			.take = on_take,
			.give_back = on_give_back,
			.caption = on_caption,
			.connected = on_connected,
		});
	} else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED || event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
		picture_studio_mode(event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED);
	} else if (event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) {
		picture_preview_changed();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		picture_stop();
		studio_exit();
		censr_link_stop(link);
		link = NULL;
		if (taken)
			unmute_ours(true); /* OBS keeps its own settings as they were, now and at next start */
	}
}

bool obs_module_load(void)
{
	pthread_mutex_init(&lock, NULL);
	muted_by_us = obs_data_array_create();
	delayed_by_us = obs_data_array_create();
	obs_register_source(&censr_sound_info);
	censr_sound = obs_source_create_private("censr_sound", "Censr", NULL);
	da_init(captions);
	obs_add_tick_callback(caption_tick, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("GiveBack"), give_back_menu, NULL);
	obs_frontend_add_event_callback(on_frontend, NULL);
	return true;
}

/* Studio's requests, once OBS's remote control is loaded */
void obs_module_post_load(void)
{
	studio_load();
}

void obs_module_unload(void)
{
	picture_stop();
	studio_unload();
	censr_link_stop(link);
	link = NULL;
	obs_remove_tick_callback(caption_tick, NULL);
	da_free(captions);
	obs_set_output_source(CHANNEL, NULL);
	obs_source_release(censr_sound);
	censr_sound = NULL;
	obs_data_array_release(muted_by_us);
	obs_data_array_release(delayed_by_us);
	pthread_mutex_destroy(&lock);
}
