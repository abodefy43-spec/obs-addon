/* Censr Studio on OBS: Studio's recording, stream and replay buffer, in Censr's own OBS.
 *
 * Censr starts its own OBS with CENSR_STUDIO=1, and only there does the add-on answer Studio's
 * "censr" requests through OBS's remote control. Each output is built as Studio asks: the video
 * encoder Studio names, with Studio's own settings (the next one Studio lists when one can't
 * start), AAC sound for each track it records, and OBS's own file writer, stream or replay
 * buffer. "status" tells what each is doing; a recording starting and ending, and a replay
 * saved, come as events. Studio checks what it asks for before asking (lib/obs_studio.dart);
 * what comes in is still held to its limits here.
 *
 * A stream's address and key are never logged or put in a message: OBS's own reason for a
 * stream ending can hold the address, so Studio gets a sentence of its own instead.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <assert.h>
#include <errno.h>
#include <obs-module.h>
#include <obs-websocket-api.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

#include "censr-studio.h"

/* a recording, the stream or the replay buffer: what it's built with, and how it's doing */
struct job {
	const char *name;
	obs_output_t *output;
	obs_service_t *service;
	obs_encoder_t *video;
	obs_encoder_t *audio[MAX_AUDIO_MIXES];
	char *encoder;   /* the video encoder's id */
	char *file;      /* the recording's newest file; the replay buffer's newest replay */
	int files;       /* the recording's files so far */
	bool split;      /* the recording was started with splitting on */
	bool on;         /* from a good start until it ends, connecting included */
	bool building;   /* a start of it is being built: one at a time */
	bool stopping;   /* asked to stop, still finishing */
	uint64_t started, connected, paused_at, paused_for;
	double seconds;  /* how long it ran, once it has ended */
	char *error;     /* why it ended, when it ended by itself */
	os_event_t *ended;
	int keep;        /* the replay buffer: the seconds it keeps */
	uint64_t bytes_then, then;
	double kbps;
};

static struct job recording = {.name = "recording"};
static struct job stream = {.name = "stream"};
static struct job replay = {.name = "replay buffer"};
static pthread_mutex_t jobs;
static obs_websocket_vendor vendor;

/* as long as Censr's own engine waits for a recording's file to finish, then for the end */
#define FINISH_MS 15000
#define FORCE_MS 5000

static void fail(obs_data_t *response, const char *error)
{
	obs_data_set_string(response, "error", error);
}

static void set_text(char **at, const char *text)
{
	bfree(*at);
	*at = text ? bstrdup(text) : NULL;
}

static bool starts_with(const char *text, const char *start)
{
	return strncmp(text, start, strlen(start)) == 0;
}

static bool ends_with(const char *text, const char *end)
{
	size_t length = strlen(text), size = strlen(end);
	return length >= size && strcmp(text + length - size, end) == 0;
}

static int clamp(long long value, int least, int most)
{
	return value < least ? least : value > most ? most : (int)value;
}

/* seconds [job] has run, its pauses left out */
static double run_seconds(const struct job *job, uint64_t now)
{
	uint64_t from = job->connected ? job->connected : job->started;
	if (!from || now < from)
		return 0;
	uint64_t paused = job->paused_for + (job->paused_at ? now - job->paused_at : 0);
	uint64_t ran = now - from;
	return ran > paused ? (double)(ran - paused) / 1e9 : 0;
}

/* why an output ended by itself, in Studio's words */
static const char *why(int code, const struct job *job)
{
	const bool live = job == &stream;
	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		return live ? "The server address didn't work. Check it in Settings › Stream." : "Studio can't save into that folder.";
	case OBS_OUTPUT_CONNECT_FAILED:
		return "Studio couldn't reach the streaming server. Check your connection, then go live again.";
	case OBS_OUTPUT_INVALID_STREAM:
		return "The streaming service didn't take the stream key. Copy it again from your streaming service.";
	case OBS_OUTPUT_DISCONNECTED:
		return "The connection to the streaming server dropped, and it couldn't reconnect.";
	case OBS_OUTPUT_UNSUPPORTED:
		return "This video encoder can't be used for that. Choose another in Settings › Output.";
	case OBS_OUTPUT_NO_SPACE:
		return "The disk is full, so it stopped. What was saved so far is kept.";
	case OBS_OUTPUT_ENCODE_ERROR:
		return "The video encoder stopped working. Choose another in Settings › Output.";
	default:
		return live ? "The stream stopped." : job == &recording ? "The recording stopped." : "The replay buffer stopped.";
	}
}

/* ---- building an output ---- */

static void drop_encoders(struct job *job)
{
	obs_encoder_release(job->video);
	job->video = NULL;
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_release(job->audio[i]);
		job->audio[i] = NULL;
	}
}

/* lets go of what an ended output was built with */
static void let_go(struct job *job)
{
	obs_output_release(job->output);
	job->output = NULL;
	obs_service_release(job->service);
	job->service = NULL;
	drop_encoders(job);
}

static bool is_video_encoder(const char *id)
{
	const char *type;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++)
		if (strcmp(type, id) == 0)
			return obs_get_encoder_type(id) == OBS_ENCODER_VIDEO;
	return false;
}

/* [job]'s encoders: [id] with Studio's [settings], and AAC sound at [kbps] for each of [tracks]
 * (bit 0 for track 1) */
static bool make_encoders(struct job *job, const char *id, obs_data_t *settings, int kbps, int tracks)
{
	struct dstr name = {0};
	dstr_printf(&name, "Censr %s video", job->name);
	job->video = obs_video_encoder_create(id, name.array, settings, NULL);
	bool made = job->video != NULL;
	if (made)
		obs_encoder_set_video(job->video, obs_get_video());
	obs_data_t *sound = obs_data_create();
	obs_data_set_int(sound, "bitrate", kbps);
	size_t count = 0;
	for (size_t mix = 0; made && mix < MAX_AUDIO_MIXES; mix++) {
		if (!(tracks & (1 << mix)))
			continue;
		dstr_printf(&name, "Censr %s sound %d", job->name, (int)mix + 1);
		obs_encoder_t *aac = obs_audio_encoder_create("ffmpeg_aac", name.array, sound, mix, NULL);
		made = aac != NULL;
		if (made) {
			obs_encoder_set_audio(aac, obs_get_audio());
			job->audio[count++] = aac;
		}
	}
	obs_data_release(sound);
	dstr_free(&name);
	if (!made || count == 0) {
		drop_encoders(job);
		return false;
	}
	return true;
}

/* starts [job]'s output with the first of the video encoders [request] lists ("video": each with
 * its "id" and "settings") that starts; the encoder's id in [response], or [could_not] */
static bool start_output(struct job *job, obs_data_t *request, int tracks, obs_data_t *response, const char *could_not)
{
	obs_data_array_t *videos = obs_data_get_array(request, "video");
	const int kbps = clamp(obs_data_get_int(request, "audio_bitrate"), 64, 320);
	const size_t count = obs_data_array_count(videos);
	bool started = false;
	for (size_t i = 0; i < count && i < 16 && !started; i++) {
		obs_data_t *video = obs_data_array_item(videos, i);
		const char *id = obs_data_get_string(video, "id");
		obs_data_t *settings = obs_data_get_obj(video, "settings");
		if (is_video_encoder(id) && make_encoders(job, id, settings, kbps, tracks)) {
			obs_output_set_video_encoder(job->output, job->video);
			for (size_t t = 0; t < MAX_AUDIO_MIXES && job->audio[t]; t++)
				obs_output_set_audio_encoder(job->output, job->audio[t], t);
			pthread_mutex_lock(&jobs);
			job->on = true;
			job->stopping = false;
			job->started = os_gettime_ns();
			job->connected = job == &stream ? 0 : job->started;
			job->paused_at = job->paused_for = 0;
			job->bytes_then = 0;
			job->then = job->started;
			job->kbps = 0;
			set_text(&job->error, NULL);
			set_text(&job->encoder, id);
			pthread_mutex_unlock(&jobs);
			os_event_reset(job->ended);
			started = obs_output_start(job->output);
			if (!started) {
				pthread_mutex_lock(&jobs);
				job->on = false;
				pthread_mutex_unlock(&jobs);
				/* never a stream's own reason: it can hold the address */
				const char *reason = job == &stream ? NULL : obs_output_get_last_error(job->output);
				blog(LOG_WARNING, "[censr] Studio's %s didn't start with %s: %s", job->name, id,
				     reason ? reason : "no reason given");
				obs_output_set_video_encoder(job->output, NULL);
				for (size_t t = 0; t < MAX_AUDIO_MIXES && job->audio[t]; t++)
					obs_output_set_audio_encoder(job->output, NULL, t);
				drop_encoders(job);
			}
		}
		obs_data_release(settings);
		obs_data_release(video);
	}
	obs_data_array_release(videos);
	if (started) {
		obs_data_set_string(response, "encoder", job->encoder);
	} else {
		fail(response, count ? could_not : "This computer has no H.264 video encoder.");
		let_go(job); /* it never ran, so nothing of it is still at work */
	}
	return started;
}

/* events said right now, which OBS's closing waits for */
static volatile long speaking;

static void say(const char *event, obs_data_t *data)
{
	pthread_mutex_lock(&jobs);
	obs_websocket_vendor to = vendor;
	if (to)
		os_atomic_inc_long(&speaking);
	pthread_mutex_unlock(&jobs);
	if (!to)
		return;
	obs_websocket_vendor_emit_event(to, event, data);
	os_atomic_dec_long(&speaking);
}

/* ---- what the outputs say (on their own threads) ---- */

static void on_stop(void *data, calldata_t *cd)
{
	struct job *job = data;
	const int code = (int)calldata_int(cd, "code");
	pthread_mutex_lock(&jobs);
	const bool asked = job->stopping;
	job->seconds = run_seconds(job, os_gettime_ns());
	job->on = false;
	job->stopping = false;
	set_text(&job->error, code == OBS_OUTPUT_SUCCESS ? NULL : why(code, job));
	obs_data_t *said = obs_data_create();
	obs_data_set_string(said, "path", job->file ? job->file : "");
	obs_data_set_string(said, "encoder", job->encoder ? job->encoder : "");
	if (job->error)
		obs_data_set_string(said, "error", job->error);
	pthread_mutex_unlock(&jobs);
	if (code != OBS_OUTPUT_SUCCESS)
		blog(LOG_WARNING, "[censr] Studio's %s ended by itself (%d)%s", job->name, code, asked ? ", as it was stopping" : "");
	/* the stop's answer goes first: OBS's remote control holds an event back while a request of
	 * the same app waits, and the stop request waits for this. Studio says a stop it asked for
	 * itself, from the answer; this event tells of a recording that ended by itself. */
	os_event_signal(job->ended);
	if (job == &recording)
		say("recordingStopped", said);
	obs_data_release(said);
}

/* the stream reached the server: its time counts from now */
static void on_connected(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct job *job = data;
	pthread_mutex_lock(&jobs);
	job->connected = os_gettime_ns();
	pthread_mutex_unlock(&jobs);
}

/* the recording went on in its next file */
static void on_next_file(void *data, calldata_t *cd)
{
	struct job *job = data;
	const char *next = calldata_string(cd, "next_file");
	pthread_mutex_lock(&jobs);
	if (next) {
		set_text(&job->file, next);
		job->files++;
	}
	pthread_mutex_unlock(&jobs);
}

static void on_saved(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct job *job = data;
	calldata_t asked = {0};
	proc_handler_call(obs_output_get_proc_handler(job->output), "get_last_replay", &asked);
	const char *path = calldata_string(&asked, "path");
	if (path && *path) {
		pthread_mutex_lock(&jobs);
		set_text(&job->file, path);
		pthread_mutex_unlock(&jobs);
		obs_data_t *said = obs_data_create();
		obs_data_set_string(said, "path", path);
		say("replaySaved", said);
		obs_data_release(said);
	}
	calldata_free(&asked);
}

static void listen_to(struct job *job)
{
	signal_handler_t *signals = obs_output_get_signal_handler(job->output);
	signal_handler_connect(signals, "stop", on_stop, job);
	if (job == &stream)
		signal_handler_connect(signals, "start", on_connected, job);
	if (job == &recording)
		signal_handler_connect(signals, "file_changed", on_next_file, job);
	if (job == &replay)
		signal_handler_connect(signals, "saved", on_saved, job);
}

/* an ended output's leftovers go before a new one is built; never while it's on, or while another
 * start of it is being built (OBS's remote control can take Studio's requests on more than one
 * thread at once) */
static bool ready(struct job *job, obs_data_t *response, const char *already)
{
	pthread_mutex_lock(&jobs);
	const bool free = !job->on && !job->building;
	if (free)
		job->building = true;
	pthread_mutex_unlock(&jobs);
	if (!free) {
		fail(response, already);
		return false;
	}
	let_go(job);
	return true;
}

/* a start of [job], built by [build] once it's [ready] */
static void start_one(struct job *job, const char *already, void (*build)(obs_data_t *, obs_data_t *),
		      obs_data_t *request, obs_data_t *response)
{
	if (!ready(job, response, already))
		return;
	build(request, response);
	pthread_mutex_lock(&jobs);
	job->building = false;
	pthread_mutex_unlock(&jobs);
}

/* ---- the recording ---- */

static void build_recording(obs_data_t *request, obs_data_t *response)
{
	const char *path = obs_data_get_string(request, "path");
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
#ifdef _WIN32
	const char *back = strrchr(path, '\\');
	if (back > slash)
		slash = back;
#endif
	if (!dot || !slash || dot < slash || strlen(path) >= 4096) {
		fail(response, "Choose a full path for the recording.");
		return;
	}
	if (os_file_exists(path)) {
		fail(response, "There's already a file with that name, and Studio won't replace it.");
		return;
	}
	const bool split = obs_data_get_bool(request, "split");
	const bool hybrid = obs_data_get_bool(request, "hybridMp4") && ends_with(path, ".mp4");
	struct dstr folder = {0};
	dstr_ncopy(&folder, path, (size_t)(slash - path));

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path);
	obs_data_set_string(settings, "muxer_settings", "");
	/* the files after the first are named as Censr's own engine names them */
	obs_data_set_string(settings, "directory", folder.array);
	obs_data_set_string(settings, "format", "Censr %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(settings, "extension", dot + 1);
	obs_data_set_bool(settings, "allow_spaces", true);
	obs_data_set_bool(settings, "allow_overwrite", false);
	obs_data_set_bool(settings, "split_file", split);
	if (split) {
		obs_data_set_int(settings, "max_time_sec", clamp(obs_data_get_int(request, "splitSeconds"), 0, 86400));
		obs_data_set_int(settings, "max_size_mb", clamp(obs_data_get_int(request, "splitMegabytes"), 0, 1048576));
	}
	recording.output = obs_output_create(hybrid ? "mp4_output" : "ffmpeg_muxer", "Censr recording", settings, NULL);
	obs_data_release(settings);
	dstr_free(&folder);
	if (!recording.output) {
		fail(response, "Studio couldn't start recording.");
		return;
	}
	listen_to(&recording);
	pthread_mutex_lock(&jobs);
	set_text(&recording.file, path);
	recording.files = 1;
	recording.split = split;
	recording.seconds = 0;
	pthread_mutex_unlock(&jobs);
	const int tracks = clamp(obs_data_get_int(request, "tracks"), 1, 63);
	if (start_output(&recording, request, tracks, response, "Studio couldn't start recording.")) {
		obs_data_t *said = obs_data_create();
		obs_data_set_string(said, "path", path);
		obs_data_set_string(said, "encoder", recording.encoder);
		say("recordingStarted", said);
		obs_data_release(said);
	}
}

static void record_start(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	start_one(&recording, "Studio is already recording.", build_recording, request, response);
}

static void put_recording(obs_data_t *into, uint64_t now)
{
	pthread_mutex_lock(&jobs);
	obs_output_t *output = obs_output_get_ref(recording.output);
	obs_data_t *r = obs_data_create();
	obs_data_set_bool(r, "active", recording.on);
	obs_data_set_bool(r, "stopping", recording.on && recording.stopping);
	if (recording.file)
		obs_data_set_string(r, "path", recording.file);
	if (recording.encoder)
		obs_data_set_string(r, "encoder", recording.encoder);
	obs_data_set_double(r, "seconds", recording.on ? run_seconds(&recording, now) : recording.seconds);
	if (recording.error)
		obs_data_set_string(r, "error", recording.error);
	obs_data_set_int(r, "files", recording.files ? recording.files : 1);
	const bool on = recording.on;
	pthread_mutex_unlock(&jobs);
	obs_data_set_int(r, "bytes", output ? (long long)obs_output_get_total_bytes(output) : 0);
	obs_data_set_int(r, "totalFrames", output ? obs_output_get_total_frames(output) : 0);
	obs_data_set_int(r, "droppedFrames", output ? obs_output_get_frames_dropped(output) : 0);
	obs_data_set_bool(r, "paused", on && output && obs_output_paused(output));
	obs_output_release(output);
	obs_data_set_obj(into, "recording", r);
	obs_data_release(r);
}

/* stops the recording and answers once its file is finished, as Censr's own engine does */
static void record_stop(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	pthread_mutex_lock(&jobs);
	const bool on = recording.on, stopping = recording.stopping;
	obs_output_t *output = on && !stopping ? obs_output_get_ref(recording.output) : NULL;
	if (output)
		recording.stopping = true;
	pthread_mutex_unlock(&jobs);
	if (!output) {
		fail(response, on ? "Studio is already finishing the recording." : "Studio isn't recording.");
		return;
	}
	obs_output_stop(output);
	if (os_event_timedwait(recording.ended, FINISH_MS) == ETIMEDOUT) {
		blog(LOG_WARNING, "[censr] Studio's recording took too long to finish; stopping it at once");
		obs_output_force_stop(output);
		os_event_timedwait(recording.ended, FORCE_MS);
	}
	obs_output_release(output);
	obs_data_t *status = obs_data_create();
	put_recording(status, os_gettime_ns());
	obs_data_t *r = obs_data_get_obj(status, "recording");
	obs_data_apply(response, r);
	obs_data_release(r);
	obs_data_release(status);
}

static void record_pause(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	const bool pause = obs_data_get_bool(request, "pause");
	pthread_mutex_lock(&jobs);
	obs_output_t *output = recording.on && !recording.stopping ? obs_output_get_ref(recording.output) : NULL;
	pthread_mutex_unlock(&jobs);
	if (!output) {
		fail(response, "Studio isn't recording.");
		return;
	}
	if (obs_output_paused(output) != pause && obs_output_pause(output, pause)) {
		const uint64_t now = os_gettime_ns();
		pthread_mutex_lock(&jobs);
		if (pause) {
			recording.paused_at = now;
		} else if (recording.paused_at) {
			recording.paused_for += now - recording.paused_at;
			recording.paused_at = 0;
		}
		pthread_mutex_unlock(&jobs);
	}
	obs_output_release(output);
}

static void record_split(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	pthread_mutex_lock(&jobs);
	obs_output_t *output = recording.on && !recording.stopping ? obs_output_get_ref(recording.output) : NULL;
	const bool split = recording.split;
	pthread_mutex_unlock(&jobs);
	if (!output) {
		fail(response, "Studio isn't recording.");
		return;
	}
	if (!split) {
		fail(response, "This recording can't be split. Turn on splitting in Settings › Output first.");
	} else {
		calldata_t cd = {0};
		proc_handler_call(obs_output_get_proc_handler(output), "split_file", &cd);
		if (!calldata_bool(&cd, "split_file_enabled"))
			fail(response, "This recording can't be split. Turn on splitting in Settings › Output first.");
		calldata_free(&cd);
	}
	obs_output_release(output);
}

/* ---- the stream ---- */

static void build_stream(obs_data_t *request, obs_data_t *response)
{
	const char *server = obs_data_get_string(request, "server");
	const char *key = obs_data_get_string(request, "key");
	/* SRT and RIST go out as MPEG-TS, as OBS sends them */
	const bool ts = starts_with(server, "srt://") || starts_with(server, "rist://");
	if (!(ts || starts_with(server, "rtmp://") || starts_with(server, "rtmps://")) || strlen(server) > 512 ||
	    strlen(key) > 512) {
		fail(response, "The server address has to start with rtmp://, rtmps://, srt:// or rist://.");
		return;
	}
	obs_data_t *service = obs_data_create();
	obs_data_set_string(service, "server", server);
	obs_data_set_string(service, "key", key);
	obs_data_set_bool(service, "use_auth", false);
	stream.service = obs_service_create_private("rtmp_custom", "Censr stream service", service);
	obs_data_release(service);

	obs_data_t *settings = obs_data_create();
	if (!ts) {
		const char *family = obs_data_get_string(request, "ipFamily");
		obs_data_set_string(settings, "bind_ip", "default");
		obs_data_set_string(settings, "ip_family",
				    strcmp(family, "IPv4") == 0 || strcmp(family, "IPv6") == 0 ? family : "IPv4+IPv6");
		obs_data_set_bool(settings, "dyn_bitrate", obs_data_get_bool(request, "dynamicBitrate"));
	}
	stream.output = obs_output_create(ts ? "ffmpeg_mpegts_muxer" : "rtmp_output", "Censr stream", settings, NULL);
	obs_data_release(settings);
	if (!stream.service || !stream.output) {
		fail(response, "Studio couldn't start the stream.");
		return;
	}
	obs_output_set_service(stream.output, stream.service);
	obs_output_set_reconnect_settings(stream.output, clamp(obs_data_get_int(request, "maxRetries"), 0, 100),
					  clamp(obs_data_get_int(request, "retryDelaySec"), 1, 60));
	obs_output_set_delay(stream.output, (uint32_t)clamp(obs_data_get_int(request, "delaySec"), 0, 1800),
			     obs_data_get_bool(request, "preserveCutoff") ? OBS_OUTPUT_DELAY_PRESERVE : 0);
	listen_to(&stream);
	start_output(&stream, request, 1, response, "Studio couldn't start the stream.");
}

static void stream_start(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	start_one(&stream, "Studio is already streaming.", build_stream, request, response);
}

static void put_stream(obs_data_t *into, uint64_t now)
{
	pthread_mutex_lock(&jobs);
	const bool on = stream.on;
	obs_output_t *output = on ? obs_output_get_ref(stream.output) : NULL;
	pthread_mutex_unlock(&jobs);
	if (!output)
		return;
	const bool again = obs_output_reconnecting(output);
	const uint64_t bytes = obs_output_get_total_bytes(output);
	obs_data_t *s = obs_data_create();
	pthread_mutex_lock(&jobs);
	/* the bitrate over at least half a second, so it doesn't jump */
	if (bytes < stream.bytes_then)
		stream.bytes_then = 0; /* a reconnect starts the count again */
	if (now - stream.then >= 500000000ULL) {
		stream.kbps = (double)(bytes - stream.bytes_then) * 8.0 / 1000.0 / ((double)(now - stream.then) / 1e9);
		stream.bytes_then = bytes;
		stream.then = now;
	}
	obs_data_set_bool(s, "active", true);
	obs_data_set_bool(s, "reconnecting", again);
	obs_data_set_double(s, "seconds", stream.connected ? run_seconds(&stream, now) : 0);
	obs_data_set_double(s, "kbps", again ? 0 : stream.kbps);
	pthread_mutex_unlock(&jobs);
	obs_data_set_int(s, "droppedFrames", obs_output_get_frames_dropped(output));
	obs_data_set_int(s, "delaySec", obs_output_get_active_delay(output));
	obs_output_release(output);
	obs_data_set_obj(into, "streaming", s);
	obs_data_release(s);
}

static void stop_job(struct job *job, obs_data_t *response, const char *off)
{
	pthread_mutex_lock(&jobs);
	obs_output_t *output = job->on ? obs_output_get_ref(job->output) : NULL;
	if (output)
		job->stopping = true;
	pthread_mutex_unlock(&jobs);
	if (!output) {
		if (off)
			fail(response, off);
		return;
	}
	obs_output_stop(output);
	obs_output_release(output);
}

static void stream_stop(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	stop_job(&stream, response, NULL);
}

/* ---- the replay buffer ---- */

static void build_replay(obs_data_t *request, obs_data_t *response)
{
	const char *format = obs_data_get_string(request, "format");
	if (strcmp(format, "mkv") != 0 && strcmp(format, "mp4") != 0 && strcmp(format, "mov") != 0 &&
	    strcmp(format, "flv") != 0) {
		fail(response, "Replays are saved as MKV, MP4, MOV or FLV.");
		return;
	}
	const int keep = clamp(obs_data_get_int(request, "seconds"), 5, 21600);
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "directory", obs_data_get_string(request, "folder"));
	/* named as Censr's own engine names replays */
	obs_data_set_string(settings, "format", "Censr replay %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(settings, "extension", format);
	obs_data_set_bool(settings, "allow_spaces", true);
	obs_data_set_int(settings, "max_time_sec", keep);
	/* the most memory it may hold, whatever the bitrate, as Censr's own engine */
	obs_data_set_int(settings, "max_size_mb", 4096);
	replay.output = obs_output_create("replay_buffer", "Censr replay buffer", settings, NULL);
	obs_data_release(settings);
	if (!replay.output) {
		fail(response, "Studio couldn't start the replay buffer.");
		return;
	}
	listen_to(&replay);
	pthread_mutex_lock(&jobs);
	replay.keep = keep;
	set_text(&replay.file, NULL);
	pthread_mutex_unlock(&jobs);
	start_output(&replay, request, 1, response, "Studio couldn't start the replay buffer.");
}

static void replay_start(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	start_one(&replay, "The replay buffer is already on.", build_replay, request, response);
}

static void replay_save(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	pthread_mutex_lock(&jobs);
	obs_output_t *output = replay.on && !replay.stopping ? obs_output_get_ref(replay.output) : NULL;
	pthread_mutex_unlock(&jobs);
	if (!output) {
		fail(response, "Turn the replay buffer on first.");
		return;
	}
	calldata_t cd = {0};
	proc_handler_call(obs_output_get_proc_handler(output), "save", &cd);
	calldata_free(&cd);
	obs_output_release(output);
}

static void replay_stop(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	stop_job(&replay, response, NULL);
}

static void put_replay(obs_data_t *into)
{
	pthread_mutex_lock(&jobs);
	if (replay.on) {
		obs_data_t *r = obs_data_create();
		obs_data_set_bool(r, "active", true);
		obs_data_set_int(r, "seconds", replay.keep);
		obs_data_set_string(r, "encoder", replay.encoder ? replay.encoder : "");
		if (replay.file)
			obs_data_set_string(r, "lastSaved", replay.file);
		obs_data_set_obj(into, "replay", r);
		obs_data_release(r);
	}
	pthread_mutex_unlock(&jobs);
}

/* ---- everything at once ---- */

static void status(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	const uint64_t now = os_gettime_ns();
	put_recording(response, now);
	put_stream(response, now);
	put_replay(response);
}

/* stops all three, waiting for the recording's file to finish */
static void stop_all(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	stop_job(&stream, response, NULL);
	stop_job(&replay, response, NULL);
	pthread_mutex_lock(&jobs);
	const bool on = recording.on;
	pthread_mutex_unlock(&jobs);
	if (on) {
		stop_job(&recording, response, NULL);
		os_event_timedwait(recording.ended, FINISH_MS);
	}
	UNUSED_PARAMETER(unused);
}

/* ---- what OBS's remote control has no request for ---- */

/* the video encoders this OBS has, Studio's to choose from */
static void encoders(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	obs_data_array_t *list = obs_data_array_create();
	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		const uint32_t caps = obs_get_encoder_caps(id);
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO || (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)))
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", id);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	obs_data_set_array(response, "video", list);
	obs_data_array_release(list);
}

static void free_space(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_data_set_int(response, "bytes", (long long)os_get_free_disk_space(obs_data_get_string(request, "folder")));
}

struct device_search {
	const char *id;
	char *name;
};

static bool find_device(void *data, const char *name, const char *id)
{
	struct device_search *search = data;
	if (strcmp(id, search->id) != 0)
		return true;
	search->name = bstrdup(name);
	return false;
}

/* where sounds Studio plays to the person ("monitoring") come out */
static void monitoring_device(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	struct device_search search = {.id = obs_data_get_string(request, "id")};
	if (strcmp(search.id, "default") == 0)
		search.name = bstrdup("Default");
	else
		obs_enum_audio_monitoring_devices(find_device, &search);
	if (!search.name || !obs_set_audio_monitoring_device(search.name, search.id))
		fail(response, "That speaker or headset isn't connected now.");
	bfree(search.name);
}

/* a sound in both ears, or each in its own */
static void mono(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "name"));
	if (!source) {
		fail(response, "There's no sound by that name.");
		return;
	}
	uint32_t flags = obs_source_get_flags(source);
	flags = obs_data_get_bool(request, "mono") ? flags | OBS_SOURCE_FLAG_FORCE_MONO : flags & ~OBS_SOURCE_FLAG_FORCE_MONO;
	obs_source_set_flags(source, flags);
	obs_source_release(source);
}

static bool add_if_mono(void *data, obs_source_t *source)
{
	if (obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", obs_source_get_name(source));
		obs_data_array_push_back(data, item);
		obs_data_release(item);
	}
	return true;
}

/* the sounds set to play in both ears */
static void monos(obs_data_t *request, obs_data_t *response, void *unused)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(unused);
	obs_data_array_t *list = obs_data_array_create();
	obs_enum_sources(add_if_mono, list);
	obs_data_set_array(response, "names", list);
	obs_data_array_release(list);
}

/* ---- starting and ending ---- */

void studio_load(void)
{
	const char *studio = getenv("CENSR_STUDIO");
	if (!studio || strcmp(studio, "1") != 0)
		return;
	pthread_mutex_init(&jobs, NULL);
	os_event_init(&recording.ended, OS_EVENT_TYPE_MANUAL);
	os_event_init(&stream.ended, OS_EVENT_TYPE_MANUAL);
	os_event_init(&replay.ended, OS_EVENT_TYPE_MANUAL);
	obs_websocket_vendor found = obs_websocket_register_vendor("censr");
	if (!found) {
		blog(LOG_WARNING, "[censr] OBS's remote control isn't here, so Studio can't record or stream");
		return;
	}
	static const struct {
		const char *type;
		obs_websocket_request_callback_function answer;
	} requests[] = {
		{"status", status},           {"record_start", record_start},
		{"record_stop", record_stop}, {"record_pause", record_pause},
		{"record_split", record_split}, {"stream_start", stream_start},
		{"stream_stop", stream_stop}, {"replay_start", replay_start},
		{"replay_save", replay_save}, {"replay_stop", replay_stop},
		{"stop_all", stop_all},       {"encoders", encoders},
		{"free_space", free_space},   {"monitoring_device", monitoring_device},
		{"mono", mono},               {"monos", monos},
	};
	for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++)
		obs_websocket_vendor_register_request(found, requests[i].type, requests[i].answer, NULL);
	pthread_mutex_lock(&jobs);
	vendor = found;
	pthread_mutex_unlock(&jobs);
	blog(LOG_INFO, "[censr] Studio's requests are ready");
}

void studio_exit(void)
{
	if (!recording.ended)
		return;
	pthread_mutex_lock(&jobs);
	vendor = NULL;
	pthread_mutex_unlock(&jobs);
	for (int waited = 0; os_atomic_load_long(&speaking) > 0 && waited < 1000; waited += 10)
		os_sleep_ms(10);
	struct job *all[] = {&recording, &stream, &replay};
	for (size_t i = 0; i < 3; i++)
		stop_job(all[i], NULL, NULL);
	os_event_timedwait(recording.ended, FINISH_MS);
}

void studio_unload(void)
{
	if (!recording.ended)
		return;
	struct job *all[] = {&recording, &stream, &replay};
	for (size_t i = 0; i < 3; i++) {
		if (all[i]->output && obs_output_active(all[i]->output))
			obs_output_force_stop(all[i]->output);
		let_go(all[i]);
		bfree(all[i]->encoder);
		bfree(all[i]->file);
		bfree(all[i]->error);
		os_event_destroy(all[i]->ended);
		all[i]->ended = NULL;
	}
	pthread_mutex_destroy(&jobs);
}
