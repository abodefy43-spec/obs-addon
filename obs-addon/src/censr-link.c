/* The link to the Censr app: see censr-link.h.
 *
 * First each side shows the other it knows the link file's secret without ever saying it (the
 * proof, as engine/obs_link.py sets it out): this side's fresh challenge, Censr's own with its
 * answer to this one, then this side's answer to Censr's. Only a Censr that answers right is
 * listened to, so a program that takes the port once Censr has gone can't speak for it.
 *
 * Then every message is one kind byte, a 4-byte little-endian length, then that many bytes. What
 * Censr sends: H (the sound's shape, JSON), A (sound), T (take over, JSON), R (give back),
 * P (still here). What the link sends: S (how it is, JSON). Anything else, or too long, and the
 * connection ends; five seconds of silence from Censr too (it says P every second).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* Windows' sockets come before anything that brings in windows.h */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#endif

#include "censr-link.h"
#include "censr-hmac.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
typedef SOCKET socket_t;
#define NO_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int socket_t;
#define NO_SOCKET (-1)
#define close_socket close
#endif

#define TOKEN_LENGTH 64
#define HELLO "CENSR2 "
#define NONCE_LENGTH 64 /* hex characters in each side's challenge */
#define ANSWER_LENGTH 64
#define PROOF_SECONDS 5
#define MOST_JSON 4096
#define MOST_AUDIO (1 << 20)
#define QUIET_SECONDS 5

struct censr_link {
	struct censr_link_events events;
	pthread_t thread;
	pthread_mutex_t send_lock;
	volatile bool stopping;
	volatile socket_t sock;
};

/* Where Censr writes the link file: its own folder in this user's settings, or where Censr says
 * when it starts this OBS itself (Censr Studio gives its OBS a settings folder of its own). NULL
 * for none: Censr's own OBS (CENSR_STUDIO) never takes the person's own OBS's link, and a place
 * Censr gave that isn't a whole path is none at all, never the usual one. */
static char *link_file(void)
{
	const char *given = getenv("CENSR_LINK_FILE");
	if (given) {
		size_t length = strlen(given);
#ifdef _WIN32
		bool whole = length > 3 && given[1] == ':' && (given[2] == '\\' || given[2] == '/');
#else
		bool whole = length > 1 && given[0] == '/';
#endif
		return whole && length < 4096 ? bstrdup(given) : NULL;
	}
	const char *studio = getenv("CENSR_STUDIO");
	if (studio && strcmp(studio, "1") == 0)
		return NULL;
#ifdef _WIN32
	return os_get_config_path_ptr("Censr/obs-link.json");
#else
	return os_get_config_path_ptr("censr/obs-link.json");
#endif
}

/* The port and secret from the link file, when it's there and looks right. */
static bool read_link_file(int *port, char token[TOKEN_LENGTH + 1])
{
	char *path = link_file();
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	if (!data)
		return false;
	long long p = obs_data_get_int(data, "port");
	const char *t = obs_data_get_string(data, "token");
	bool good = p > 0 && p < 65536 && t && strlen(t) == TOKEN_LENGTH;
	for (size_t i = 0; good && i < TOKEN_LENGTH; i++)
		good = isxdigit((unsigned char)t[i]) != 0;
	if (good) {
		*port = (int)p;
		memcpy(token, t, TOKEN_LENGTH + 1);
	}
	obs_data_release(data);
	return good;
}

static bool send_all(socket_t sock, const void *data, size_t size)
{
	const char *at = data;
	while (size > 0) {
		int sent = send(sock, at, (int)size, 0);
		if (sent <= 0)
			return false;
		at += sent;
		size -= (size_t)sent;
	}
	return true;
}

static bool read_all(socket_t sock, void *data, size_t size)
{
	char *at = data;
	while (size > 0) {
		int got = recv(sock, at, (int)size, 0);
		if (got <= 0)
			return false;
		at += got;
		size -= (size_t)got;
	}
	return true;
}

static socket_t connect_to(int port)
{
	socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == NO_SOCKET)
		return NO_SOCKET;
	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)port);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* this computer only */
	if (connect(sock, (struct sockaddr *)&to, sizeof(to)) != 0) {
		close_socket(sock);
		return NO_SOCKET;
	}
	/* Censr says something every second: this long with nothing means it's gone */
#ifdef _WIN32
	DWORD wait = QUIET_SECONDS * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof(wait));
#else
	struct timeval wait = {QUIET_SECONDS, 0};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
#endif
	return sock;
}

/* A fresh challenge: random bytes from the system's own source of secrets, as lowercase hex. */
static bool fresh_challenge(char hex[NONCE_LENGTH + 1])
{
	uint8_t bytes[NONCE_LENGTH / 2];
#ifdef _WIN32
	if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, bytes, (ULONG)sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
		return false;
#else
	if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes))
		return false;
#endif
	for (size_t i = 0; i < sizeof(bytes); i++)
		snprintf(hex + i * 2, 3, "%02x", bytes[i]);
	return true;
}

/* One line from Censr, without its newline: shorter than [most], and whole by [by_ns]. */
static bool read_line(socket_t sock, char *line, size_t most, uint64_t by_ns)
{
	for (size_t at = 0; at + 1 < most && os_gettime_ns() < by_ns; at++) {
		if (!read_all(sock, line + at, 1))
			return false;
		if (line[at] == '\n') {
			line[at] = 0;
			return true;
		}
	}
	return false;
}

/* The proof (see the top), with the link file's [token]: whether Censr answered right, and this
 * side's answer went out. */
static bool prove(socket_t sock, const char *token)
{
	char mine[NONCE_LENGTH + 1], line[256], said[256], answer[ANSWER_LENGTH + 2];
	const uint64_t by_ns = os_gettime_ns() + PROOF_SECONDS * 1000000000ULL;
	const size_t hello = strlen(HELLO);
	if (!fresh_challenge(mine))
		return false;
	snprintf(said, sizeof(said), HELLO "%s\n", mine);
	if (!send_all(sock, said, strlen(said)) || !read_line(sock, line, sizeof(line), by_ns))
		return false;
	/* CENSR2 <Censr's challenge> <its answer> */
	const char *theirs = line + hello, *its_answer = theirs + NONCE_LENGTH + 1;
	if (strlen(line) != hello + NONCE_LENGTH + 1 + ANSWER_LENGTH || strncmp(line, HELLO, hello) != 0 ||
	    theirs[NONCE_LENGTH] != ' ')
		return false;
	for (size_t i = 0; i < NONCE_LENGTH; i++)
		if (!strchr("0123456789abcdef", theirs[i]))
			return false;
	snprintf(said, sizeof(said), "censr-engine %s %.*s", mine, NONCE_LENGTH, theirs);
	censr_hmac_hex(token, TOKEN_LENGTH, said, strlen(said), answer);
	if (!censr_same(answer, its_answer, ANSWER_LENGTH))
		return false; /* not Censr: whoever it is never hears from this side again */
	snprintf(said, sizeof(said), "censr-addon %s %.*s", mine, NONCE_LENGTH, theirs);
	censr_hmac_hex(token, TOKEN_LENGTH, said, strlen(said), answer);
	answer[ANSWER_LENGTH] = '\n';
	return send_all(sock, answer, ANSWER_LENGTH + 1);
}

/* A JSON payload as obs_data (or NULL), from bytes that aren't NUL-terminated. */
static obs_data_t *json_of(const uint8_t *bytes, size_t size)
{
	char *text = bmalloc(size + 1);
	memcpy(text, bytes, size);
	text[size] = 0;
	obs_data_t *data = obs_data_create_from_json(text);
	bfree(text);
	return data;
}

/* One connection's messages, until it ends. */
static void listen_to(struct censr_link *link, socket_t sock)
{
	uint8_t *body = bmalloc(MOST_AUDIO);
	for (;;) {
		uint8_t head[5];
		if (link->stopping || !read_all(sock, head, sizeof(head)))
			break;
		char kind = (char)head[0];
		uint32_t size = (uint32_t)head[1] | ((uint32_t)head[2] << 8) | ((uint32_t)head[3] << 16) |
				((uint32_t)head[4] << 24);
		size_t most = kind == 'A' ? MOST_AUDIO : (kind == 'H' || kind == 'T' || kind == 'C') ? MOST_JSON : 0;
		if ((kind != 'A' && kind != 'H' && kind != 'T' && kind != 'C' && kind != 'R' && kind != 'P') || size > most)
			break; /* not something Censr says */
		if (size && !read_all(sock, body, size))
			break;
		const struct censr_link_events *on = &link->events;
		if (kind == 'A') {
			if (on->audio && size % 2 == 0)
				on->audio(body, size, on->data);
		} else if (kind == 'H') {
			obs_data_t *shape = json_of(body, size);
			if (!shape)
				break;
			long long rate = obs_data_get_int(shape, "rate");
			long long channels = obs_data_get_int(shape, "channels");
			obs_data_release(shape);
			if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
				break;
			if (on->shape)
				on->shape((uint32_t)rate, (uint32_t)channels, on->data);
		} else if (kind == 'T') {
			obs_data_t *take = json_of(body, size);
			if (!take)
				break;
			bool computer = obs_data_get_bool(take, "computer");
			long long delay_ms = obs_data_get_int(take, "delay_ms");
			obs_data_release(take);
			if (delay_ms < 0 || delay_ms > 10000)
				break;
			if (on->take)
				on->take(computer, (int)delay_ms, on->data);
		} else if (kind == 'C') {
			obs_data_t *caption = json_of(body, size);
			if (!caption)
				break;
			const char *text = obs_data_get_string(caption, "text");
			long long after_ms = obs_data_get_int(caption, "after_ms");
			if (on->caption && text && after_ms >= 0 && after_ms <= 10000)
				on->caption(text, (int)after_ms, on->data);
			obs_data_release(caption);
		} else if (kind == 'R') {
			if (on->give_back)
				on->give_back(on->data);
		}
	}
	bfree(body);
}

static void *run(void *param)
{
	struct censr_link *link = param;
	os_set_thread_name("censr-link");
	while (!link->stopping) {
		int port = 0;
		char token[TOKEN_LENGTH + 1];
		socket_t sock = read_link_file(&port, token) ? connect_to(port) : NO_SOCKET;
		if (sock == NO_SOCKET) {
			for (int i = 0; i < 10 && !link->stopping; i++)
				os_sleep_ms(100);
			continue;
		}
		if (prove(sock, token)) {
			pthread_mutex_lock(&link->send_lock);
			link->sock = sock;
			pthread_mutex_unlock(&link->send_lock);
			if (link->events.connected)
				link->events.connected(true, link->events.data);
			listen_to(link, sock);
			pthread_mutex_lock(&link->send_lock);
			link->sock = NO_SOCKET;
			pthread_mutex_unlock(&link->send_lock);
			if (link->events.connected)
				link->events.connected(false, link->events.data);
		}
		close_socket(sock);
		os_sleep_ms(500);
	}
	return NULL;
}

struct censr_link *censr_link_start(const struct censr_link_events *events)
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	struct censr_link *link = bzalloc(sizeof(*link));
	link->events = *events;
	link->sock = NO_SOCKET;
	pthread_mutex_init(&link->send_lock, NULL);
	if (pthread_create(&link->thread, NULL, run, link) != 0) {
		pthread_mutex_destroy(&link->send_lock);
		bfree(link);
		return NULL;
	}
	return link;
}

void censr_link_stop(struct censr_link *link)
{
	if (!link)
		return;
	link->stopping = true;
	pthread_mutex_lock(&link->send_lock);
	if (link->sock != NO_SOCKET)
		shutdown(link->sock, 2); /* wakes the reading thread */
	pthread_mutex_unlock(&link->send_lock);
	pthread_join(link->thread, NULL);
	pthread_mutex_destroy(&link->send_lock);
	bfree(link);
#ifdef _WIN32
	WSACleanup();
#endif
}

void censr_link_say(struct censr_link *link, bool taken)
{
	if (!link)
		return;
	struct dstr said = {0};
	dstr_printf(&said, "{\"taken\": %s, \"obs\": \"%s\"}", taken ? "true" : "false", obs_get_version_string());
	uint8_t head[5] = {'S', (uint8_t)(said.len & 0xff), (uint8_t)((said.len >> 8) & 0xff),
			   (uint8_t)((said.len >> 16) & 0xff), (uint8_t)((said.len >> 24) & 0xff)};
	pthread_mutex_lock(&link->send_lock);
	if (link->sock != NO_SOCKET && send_all(link->sock, head, sizeof(head)))
		send_all(link->sock, said.array, said.len);
	pthread_mutex_unlock(&link->send_lock);
	dstr_free(&said);
}
