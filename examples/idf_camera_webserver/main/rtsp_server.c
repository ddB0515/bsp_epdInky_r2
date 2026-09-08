/*
 * A small RTSP/RTP server for the epdInky ESP32-P4 camera example.
 *
 * Espressif publish no RTSP component, so this implements the subset of
 * RFC 2326 that real clients (VLC, ffmpeg/ffplay, most NVR software) actually
 * use: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN and the two keep-alive
 * methods. Video is carried as H.264 over RTP using the RFC 6184 payload
 * format, with FU-A fragmentation for NALs that exceed the MTU.
 *
 * Four mount points, one per sensor mode:
 *
 *   rtsp://<board>:8554/stream1   1920x1080
 *   rtsp://<board>:8554/stream2   1280x720
 *   rtsp://<board>:8554/stream3   800x800
 *   rtsp://<board>:8554/stream4   640x480
 *
 * Each switches the sensor to its own resolution. Scaling is not an option:
 * the PPA cannot run while the hardware H.264 encoder is open, so the encoder
 * has to be fed the sensor's native size. See the note in pipeline_start().
 *
 * Only one consumer can stream at a time, because there is one capture queue
 * and one encoder: starting an RTSP session takes the camera from the MJPEG
 * preview, and vice versa.
 *
 * Both RTP-over-UDP and RTP-interleaved-over-TCP are supported, because
 * clients differ: ffmpeg defaults to UDP, and TCP is what works through NAT.
 *
 * The whole dialogue plus the streaming loop runs in a single task. That
 * avoids any locking between "client sent TEARDOWN" and "we are mid-frame",
 * which is the usual source of bugs in servers like this: while playing, the
 * control socket is polled non-blocking between frames.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_camera.h"
#include "app_h264.h"
#include "rtsp_server.h"

static const char *TAG = "rtsp";

/* Keep RTP payloads inside a single Ethernet/Wi-Fi frame. 1400 leaves room for
 * the RTP, UDP and IP headers without risking fragmentation. */
#define RTP_MAX_PAYLOAD   1400
#define RTP_HEADER_SIZE     12
#define RTP_PAYLOAD_TYPE    96   /* first dynamic type */
#define RTP_CLOCK_HZ     90000   /* fixed by the H.264 RTP payload spec */

#define RTSP_REQUEST_MAX  2048
#define RTSP_RESPONSE_MAX 1024

#define H264_I_PERIOD 30

typedef struct {
	const char *mount;
	uint32_t    width;
	uint32_t    height;
	uint32_t    bitrate;   /* scaled to the pixel count, roughly 0.1 bit/pixel/frame */
} rtsp_endpoint_t;

/*
 * One mount per sensor mode. Each endpoint switches the sensor to its own
 * resolution, because the H.264 encoder can only be fed the native size - see
 * the note in pipeline_start().
 *
 * These must be modes the SC2336 actually has. It offers 1920x1080, 1280x720,
 * 1024x600, 800x800 and 640x480; there is no 800x600, so /stream3 is the
 * sensor's square 800x800 mode.
 */
static const rtsp_endpoint_t s_endpoints[] = {
	{ "/stream1", 1920, 1080, 4000000 },
	{ "/stream2", 1280,  720, 2500000 },
	{ "/stream3",  800,  800, 1500000 },
	{ "/stream4",  640,  480,  800000 },
};

#define ENDPOINT_COUNT (sizeof(s_endpoints) / sizeof(s_endpoints[0]))

typedef struct {
	int   sock;             /* RTSP control connection                     */
	const rtsp_endpoint_t *ep;

	bool  interleaved;      /* RTP inside the RTSP connection, RFC 2326 10.12 */
	uint8_t rtp_channel;
	uint8_t rtcp_channel;

	int   rtp_sock;         /* UDP mode only */
	struct sockaddr_in rtp_dest;

	uint32_t ssrc;
	uint16_t seq;
	uint32_t session_id;

	bool  playing;
	bool  pipeline_up;
	uint32_t camera_token;
	int64_t  start_us;
} rtsp_session_t;

static TaskHandle_t s_task;
static bool         s_running;
/* Kept at file scope so rtsp_server_stop() can close it: that is what unblocks
 * the accept() the task is parked in. */
static int          s_listen_sock = -1;
static volatile bool s_playing;
static volatile uint32_t s_play_w, s_play_h;

/* Scratch buffer for building RTP packets, reused for every packet. */
static uint8_t s_packet[RTP_MAX_PAYLOAD + RTP_HEADER_SIZE + 4];

/* ===========================================================================
 * Small helpers
 * ========================================================================= */

static const char *base64_chars =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len) { v |= (uint32_t)in[i + 1] << 8; }
		if (i + 2 < len) { v |= in[i + 2]; }

		if (o + 4 >= out_size) {
			break;
		}
		out[o++] = base64_chars[(v >> 18) & 0x3F];
		out[o++] = base64_chars[(v >> 12) & 0x3F];
		out[o++] = (i + 1 < len) ? base64_chars[(v >> 6) & 0x3F] : '=';
		out[o++] = (i + 2 < len) ? base64_chars[v & 0x3F] : '=';
	}
	out[o] = '\0';
	return o;
}

/* TCP writes can be short, especially with a full send buffer. */
static int send_all(int sock, const uint8_t *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int n = send(sock, data + sent, len - sent, 0);
		if (n <= 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		sent += n;
	}
	return 0;
}

/* Case-insensitive header lookup; returns a pointer just past "Name:". */
static const char *find_header(const char *req, const char *name)
{
	size_t name_len = strlen(name);
	for (const char *p = req; *p; p++) {
		if ((p == req || p[-1] == '\n') && strncasecmp(p, name, name_len) == 0 &&
		    p[name_len] == ':') {
			p += name_len + 1;
			while (*p == ' ' || *p == '\t') {
				p++;
			}
			return p;
		}
	}
	return NULL;
}

static int parse_cseq(const char *req)
{
	const char *h = find_header(req, "CSeq");
	return h ? atoi(h) : 0;
}

static const rtsp_endpoint_t *match_endpoint(const char *url)
{
	for (size_t i = 0; i < ENDPOINT_COUNT; i++) {
		if (strstr(url, s_endpoints[i].mount)) {
			return &s_endpoints[i];
		}
	}
	return NULL;
}

static void send_response(rtsp_session_t *s, int cseq, const char *status,
                          const char *extra_headers, const char *body)
{
	char   resp[RTSP_RESPONSE_MAX];
	size_t n = 0;

	n += snprintf(resp + n, sizeof(resp) - n,
	              "RTSP/1.0 %s\r\nCSeq: %d\r\nServer: epdInky-P4\r\n", status, cseq);

	if (extra_headers && *extra_headers) {
		n += snprintf(resp + n, sizeof(resp) - n, "%s", extra_headers);
	}
	if (body && *body) {
		n += snprintf(resp + n, sizeof(resp) - n,
		              "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
		              (unsigned)strlen(body), body);
	} else {
		n += snprintf(resp + n, sizeof(resp) - n, "\r\n");
	}

	if (n >= sizeof(resp)) {
		ESP_LOGE(TAG, "response truncated");
		return;
	}
	send_all(s->sock, (const uint8_t *)resp, n);
}

/* ===========================================================================
 * Pipeline
 * ========================================================================= */

static void pipeline_stop(rtsp_session_t *s)
{
	if (!s->pipeline_up) {
		return;
	}
	app_h264_close();
	app_camera_stream_release(s->camera_token);
	s->camera_token = 0;
	s->pipeline_up  = false;
	s_playing       = false;
}

/*
 * Bring up camera + encoder for this session's resolution.
 *
 * Idempotent, because clients vary: some DESCRIBE then SETUP then PLAY, others
 * skip DESCRIBE entirely.
 */
static esp_err_t pipeline_start(rtsp_session_t *s)
{
	if (s->pipeline_up) {
		return ESP_OK;
	}

	/*
	 * HARDWARE LIMITATION: the PPA scaler and the hardware H.264 encoder
	 * cannot be used at the same time. Once the encoder is open, the PPA's
	 * completion interrupt never arrives and ppa_do_scale_rotate_mirror()
	 * blocks forever. Verified in isolation: the PPA alone (the MJPEG
	 * preview, which scales and converts to RGB565) works, the encoder alone
	 * works, and together they deadlock on the first scale.
	 *
	 * So the encoder is always fed the sensor's native output, and each
	 * endpoint switches the sensor to the resolution it advertises.
	 */
	s->camera_token = app_camera_stream_acquire(APP_STREAM_H264,
	                                            s->ep->width, s->ep->height);
	ESP_RETURN_ON_FALSE(s->camera_token != 0, ESP_FAIL, TAG,
	                    "could not take the camera for %s at %" PRIu32 "x%" PRIu32,
	                    s->ep->mount, s->ep->width, s->ep->height);

	/* Use what the sensor actually produced, not what we asked for. */
	uint32_t w, h;
	app_camera_get_size(&w, &h);

	esp_err_t err = app_h264_open(w, h, s->ep->bitrate, H264_I_PERIOD);
	if (err != ESP_OK) {
		app_camera_stream_release(s->camera_token);
		s->camera_token = 0;
		return err;
	}

	s->pipeline_up = true;
	s_play_w = w;
	s_play_h = h;
	ESP_LOGI(TAG, "pipeline up for %s at %" PRIu32 "x%" PRIu32, s->ep->mount, w, h);
	return ESP_OK;
}

/*
 * Encode frames until SPS and PPS have been seen.
 *
 * The SDP has to carry the parameter sets, but the encoder only emits them
 * with the first keyframe, so a few frames are pushed through before DESCRIBE
 * can be answered. In practice the very first frame is an IDR and this loops
 * exactly once.
 */
static esp_err_t prime_parameter_sets(void)
{
	for (int attempt = 0; attempt < 10; attempt++) {
		if (app_h264_get_parameter_sets(NULL, NULL, NULL, NULL) == ESP_OK) {
			return ESP_OK;
		}

		app_camera_frame_t raw;
		if (app_camera_capture_yuv(&raw) != ESP_OK) {
			continue;
		}
		app_h264_frame_t enc;
		app_h264_encode(raw.data, raw.len, &enc);
		app_camera_release();
	}
	return app_h264_get_parameter_sets(NULL, NULL, NULL, NULL);
}

/* ===========================================================================
 * RTP
 * ========================================================================= */

static int rtp_send(rtsp_session_t *s, const uint8_t *payload, size_t len,
                    uint32_t timestamp, bool marker)
{
	size_t offset = 0;

	/* Interleaved mode wraps each packet in a 4-byte framing header so it can
	 * share the RTSP TCP connection. */
	if (s->interleaved) {
		s_packet[0] = '$';
		s_packet[1] = s->rtp_channel;
		s_packet[2] = (uint8_t)((len + RTP_HEADER_SIZE) >> 8);
		s_packet[3] = (uint8_t)((len + RTP_HEADER_SIZE) & 0xFF);
		offset = 4;
	}

	uint8_t *rtp = &s_packet[offset];
	rtp[0] = 0x80;                                        /* version 2 */
	rtp[1] = RTP_PAYLOAD_TYPE | (marker ? 0x80 : 0x00);
	rtp[2] = (uint8_t)(s->seq >> 8);
	rtp[3] = (uint8_t)(s->seq & 0xFF);
	rtp[4] = (uint8_t)(timestamp >> 24);
	rtp[5] = (uint8_t)(timestamp >> 16);
	rtp[6] = (uint8_t)(timestamp >> 8);
	rtp[7] = (uint8_t)(timestamp);
	rtp[8]  = (uint8_t)(s->ssrc >> 24);
	rtp[9]  = (uint8_t)(s->ssrc >> 16);
	rtp[10] = (uint8_t)(s->ssrc >> 8);
	rtp[11] = (uint8_t)(s->ssrc);
	s->seq++;

	memcpy(rtp + RTP_HEADER_SIZE, payload, len);
	size_t total = offset + RTP_HEADER_SIZE + len;

	if (s->interleaved) {
		return send_all(s->sock, s_packet, total);
	}
	int n = sendto(s->rtp_sock, s_packet, total, 0,
	               (struct sockaddr *)&s->rtp_dest, sizeof(s->rtp_dest));
	return (n < 0) ? -1 : 0;
}

/*
 * Send one NAL unit, fragmenting if it does not fit in a packet.
 *
 * Small NALs go out whole (single NAL unit mode). Larger ones are split with
 * FU-A: the original header's F and NRI bits are preserved in the indicator,
 * and the type moves into the FU header along with start/end markers.
 */
static int rtp_send_nal(rtsp_session_t *s, const uint8_t *nal, size_t len,
                        uint32_t timestamp, bool last_in_frame)
{
	if (len == 0) {
		return 0;
	}

	if (len <= RTP_MAX_PAYLOAD) {
		return rtp_send(s, nal, len, timestamp, last_in_frame);
	}

	uint8_t indicator = (nal[0] & 0xE0) | 28;  /* FU-A */
	uint8_t nal_type  = nal[0] & 0x1F;

	const uint8_t *data      = nal + 1;        /* header is not resent */
	size_t         remaining = len - 1;
	bool           first     = true;

	uint8_t frag[RTP_MAX_PAYLOAD];
	while (remaining > 0) {
		size_t chunk = remaining;
		if (chunk > RTP_MAX_PAYLOAD - 2) {
			chunk = RTP_MAX_PAYLOAD - 2;
		}
		bool last = (chunk == remaining);

		frag[0] = indicator;
		frag[1] = nal_type | (first ? 0x80 : 0x00) | (last ? 0x40 : 0x00);
		memcpy(frag + 2, data, chunk);

		if (rtp_send(s, frag, chunk + 2, timestamp, last && last_in_frame) != 0) {
			return -1;
		}

		data      += chunk;
		remaining -= chunk;
		first      = false;
	}
	return 0;
}

/* Find the next Annex-B start code at or after *pos, returning its length. */
static const uint8_t *next_start_code(const uint8_t *buf, size_t len, size_t *sc_len)
{
	for (size_t i = 0; i + 3 <= len; i++) {
		if (buf[i] == 0 && buf[i + 1] == 0) {
			if (buf[i + 2] == 1) {
				*sc_len = 3;
				return &buf[i];
			}
			if (i + 4 <= len && buf[i + 2] == 0 && buf[i + 3] == 1) {
				*sc_len = 4;
				return &buf[i];
			}
		}
	}
	return NULL;
}

/* Split an access unit on start codes and send each NAL as RTP. */
static int rtp_send_access_unit(rtsp_session_t *s, const uint8_t *buf, size_t len,
                                uint32_t timestamp)
{
	size_t sc_len = 0;
	const uint8_t *p = next_start_code(buf, len, &sc_len);
	if (!p) {
		return rtp_send_nal(s, buf, len, timestamp, true);
	}

	while (p) {
		const uint8_t *nal = p + sc_len;
		size_t remaining = len - (nal - buf);

		size_t next_sc_len = 0;
		const uint8_t *next = next_start_code(nal, remaining, &next_sc_len);

		size_t nal_len = next ? (size_t)(next - nal) : remaining;
		if (rtp_send_nal(s, nal, nal_len, timestamp, next == NULL) != 0) {
			return -1;
		}

		p      = next;
		sc_len = next_sc_len;
	}
	return 0;
}

/* ===========================================================================
 * RTSP methods
 * ========================================================================= */

static void handle_options(rtsp_session_t *s, int cseq)
{
	send_response(s, cseq,	"200 OK",
	              "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
	              NULL);
}

static void handle_describe(rtsp_session_t *s, int cseq, const char *url)
{
	if (pipeline_start(s) != ESP_OK) {
		send_response(s, cseq, "503 Service Unavailable", NULL, NULL);
		return;
	}
	if (prime_parameter_sets() != ESP_OK) {
		ESP_LOGE(TAG, "no SPS/PPS available");
		send_response(s, cseq, "500 Internal Server Error", NULL, NULL);
		return;
	}

	const uint8_t *sps, *pps;
	size_t sps_len, pps_len;
	app_h264_get_parameter_sets(&sps, &sps_len, &pps, &pps_len);

	char sps_b64[128], pps_b64[64];
	base64_encode(sps, sps_len, sps_b64, sizeof(sps_b64));
	base64_encode(pps, pps_len, pps_b64, sizeof(pps_b64));

	/* profile_idc / constraint flags / level_idc, straight out of the SPS. */
	char profile[8] = "42001f";
	if (sps_len >= 4) {
		snprintf(profile, sizeof(profile), "%02x%02x%02x", sps[1], sps[2], sps[3]);
	}

	uint32_t w, h;
	app_camera_get_size(&w, &h);

	char sdp[768];
	snprintf(sdp, sizeof(sdp),
	         "v=0\r\n"
	         "o=- 0 0 IN IP4 0.0.0.0\r\n"
	         "s=epdInky ESP32-P4 camera\r\n"
	         "i=%" PRIu32 "x%" PRIu32 "\r\n"
	         "c=IN IP4 0.0.0.0\r\n"
	         "t=0 0\r\n"
	         "a=tool:epdInky\r\n"
	         "a=type:broadcast\r\n"
	         "a=control:*\r\n"
	         "m=video 0 RTP/AVP %d\r\n"
	         "a=rtpmap:%d H264/%d\r\n"
	         "a=fmtp:%d packetization-mode=1;profile-level-id=%s;"
	         "sprop-parameter-sets=%s,%s\r\n"
	         "a=control:trackID=0\r\n",
	         w, h, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, RTP_CLOCK_HZ,
	         RTP_PAYLOAD_TYPE, profile, sps_b64, pps_b64);

	char extra[128];
	snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
	send_response(s, cseq, "200 OK", extra, sdp);
}

static void handle_setup(rtsp_session_t *s, int cseq, const char *req)
{
	const char *transport = find_header(req, "Transport");
	if (!transport) {
		send_response(s, cseq, "400 Bad Request", NULL, NULL);
		return;
	}

	char extra[256];
	s->session_id = s->session_id ? s->session_id : (esp_random() | 1);

	if (strstr(transport, "RTP/AVP/TCP")) {
		s->interleaved  = true;
		s->rtp_channel  = 0;
		s->rtcp_channel = 1;

		const char *il = strstr(transport, "interleaved=");
		if (il) {
			int a = 0, b = 1;
			if (sscanf(il, "interleaved=%d-%d", &a, &b) >= 1) {
				s->rtp_channel  = (uint8_t)a;
				s->rtcp_channel = (uint8_t)b;
			}
		}
		snprintf(extra, sizeof(extra),
		         "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n"
		         "Session: %08" PRIX32 ";timeout=60\r\n",
		         s->rtp_channel, s->rtcp_channel, s->session_id);
		ESP_LOGI(TAG, "SETUP %s over TCP (interleaved %u-%u)",
		         s->ep->mount, s->rtp_channel, s->rtcp_channel);
	} else {
		int client_rtp = 0, client_rtcp = 0;
		const char *cp = strstr(transport, "client_port=");
		if (!cp || sscanf(cp, "client_port=%d-%d", &client_rtp, &client_rtcp) < 1) {
			send_response(s, cseq, "461 Unsupported Transport", NULL, NULL);
			return;
		}

		/* Reply to whichever address the control connection came from. */
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		if (getpeername(s->sock, (struct sockaddr *)&peer, &peer_len) != 0) {
			send_response(s, cseq, "500 Internal Server Error", NULL, NULL);
			return;
		}

		if (s->rtp_sock >= 0) {
			close(s->rtp_sock);
		}
		s->rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s->rtp_sock < 0) {
			send_response(s, cseq, "500 Internal Server Error", NULL, NULL);
			return;
		}

		struct sockaddr_in local = {
			.sin_family      = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port        = 0,
		};
		bind(s->rtp_sock, (struct sockaddr *)&local, sizeof(local));

		socklen_t local_len = sizeof(local);
		getsockname(s->rtp_sock, (struct sockaddr *)&local, &local_len);

		s->interleaved = false;
		s->rtp_dest    = peer;
		s->rtp_dest.sin_port = htons((uint16_t)client_rtp);

		snprintf(extra, sizeof(extra),
		         "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
		         "Session: %08" PRIX32 ";timeout=60\r\n",
		         client_rtp, client_rtcp,
		         ntohs(local.sin_port), ntohs(local.sin_port) + 1,
		         s->session_id);
		ESP_LOGI(TAG, "SETUP %s over UDP to port %d", s->ep->mount, client_rtp);
	}

	send_response(s, cseq, "200 OK", extra, NULL);
}

static void handle_play(rtsp_session_t *s, int cseq, const char *url)
{
	if (pipeline_start(s) != ESP_OK) {
		send_response(s, cseq, "503 Service Unavailable", NULL, NULL);
		return;
	}

	char extra[192];
	snprintf(extra, sizeof(extra),
	         "Session: %08" PRIX32 "\r\nRange: npt=0.000-\r\n"
	         "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=0\r\n",
	         s->session_id, url, s->seq);
	send_response(s, cseq, "200 OK", extra, NULL);

	s->playing  = true;
	s->start_us = esp_timer_get_time();
	s_playing   = true;
	ESP_LOGI(TAG, "PLAY %s", s->ep->mount);
}

/* ===========================================================================
 * Session loop
 * ========================================================================= */

static bool handle_request(rtsp_session_t *s, const char *req)
{
	char method[24] = {0};
	char url[192]   = {0};
	if (sscanf(req, "%23s %191s", method, url) != 2) {
		return true;
	}

	int cseq = parse_cseq(req);

	/* The endpoint is chosen by the first URL that names one, and then kept:
	 * later requests in the dialogue may carry a trackID suffix. */
	if (!s->ep) {
		s->ep = match_endpoint(url);
	}
	if (!s->ep && (strcasecmp(method, "DESCRIBE") == 0 || strcasecmp(method, "SETUP") == 0)) {
		ESP_LOGW(TAG, "unknown mount point in '%s'", url);
		send_response(s, cseq, "404 Not Found", NULL, NULL);
		return true;
	}

	if (strcasecmp(method, "OPTIONS") == 0) {
		handle_options(s, cseq);
	} else if (strcasecmp(method, "DESCRIBE") == 0) {
		handle_describe(s, cseq, url);
	} else if (strcasecmp(method, "SETUP") == 0) {
		handle_setup(s, cseq, req);
	} else if (strcasecmp(method, "PLAY") == 0) {
		handle_play(s, cseq, url);
	} else if (strcasecmp(method, "PAUSE") == 0) {
		s->playing = false;
		s_playing  = false;
		send_response(s, cseq, "200 OK", NULL, NULL);
	} else if (strcasecmp(method, "TEARDOWN") == 0) {
		send_response(s, cseq, "200 OK", NULL, NULL);
		ESP_LOGI(TAG, "TEARDOWN");
		return false;
	} else if (strcasecmp(method, "GET_PARAMETER") == 0 ||
	           strcasecmp(method, "SET_PARAMETER") == 0) {
		send_response(s, cseq, "200 OK", NULL, NULL);   /* keep-alive */
	} else {
		send_response(s, cseq, "501 Not Implemented", NULL, NULL);
	}
	return true;
}

/*
 * Read and dispatch any pending control messages.
 *
 * Used both for the blocking pre-PLAY dialogue and, with timeout_ms = 0, as a
 * poll between frames while streaming.
 *
 * @return false when the connection should be closed.
 */
static bool service_control(rtsp_session_t *s, int timeout_ms)
{
	fd_set  rfds;
	FD_ZERO(&rfds);
	FD_SET(s->sock, &rfds);

	struct timeval tv = {
		.tv_sec  = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	int rc = select(s->sock + 1, &rfds, NULL, NULL, &tv);
	if (rc <= 0) {
		return true;   /* nothing waiting, or a benign interruption */
	}

	char req[RTSP_REQUEST_MAX];
	int  n = recv(s->sock, req, sizeof(req) - 1, 0);
	if (n <= 0) {
		return false;  /* client closed */
	}
	req[n] = '\0';

	/* Interleaved RTCP from the client can share the connection; skip it. */
	if (req[0] == '$') {
		return true;
	}

	return handle_request(s, req);
}

static void stream_one_frame(rtsp_session_t *s, bool *ok)
{
	app_camera_frame_t raw;
	if (app_camera_capture_yuv(&raw) != ESP_OK) {
		return;
	}

	app_h264_frame_t enc;
	esp_err_t err = app_h264_encode(raw.data, raw.len, &enc);
	app_camera_release();

	if (err != ESP_OK) {
		return;
	}

	/* Deriving the timestamp from the wall clock rather than a frame counter
	 * keeps playback smooth even though the frame rate is not exactly
	 * constant. */
	int64_t  elapsed_us = esp_timer_get_time() - s->start_us;
	uint32_t timestamp  = (uint32_t)((elapsed_us * RTP_CLOCK_HZ) / 1000000);

	if (rtp_send_access_unit(s, enc.data, enc.len, timestamp) != 0) {
		ESP_LOGW(TAG, "send failed, dropping the client");
		*ok = false;
	}
}

static void session_run(int sock)
{
	rtsp_session_t s = {
		.sock     = sock,
		.rtp_sock = -1,
		.ssrc     = esp_random(),
		.seq      = (uint16_t)esp_random(),
	};

	int flag = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	bool alive = true;
	while (alive && s_running) {
		if (!s.playing) {
			/* Idle: block for a while so the task is not spinning. */
			alive = service_control(&s, 500);
			continue;
		}

		alive = service_control(&s, 0);
		if (!alive) {
			break;
		}

		/* Another consumer (the MJPEG preview, or a second RTSP client) may
		 * have taken the camera; stop cleanly if so. */
		if (!app_camera_stream_is_owner(s.camera_token)) {
			ESP_LOGI(TAG, "camera taken by another consumer, ending session");
			break;
		}

		stream_one_frame(&s, &alive);
	}

	pipeline_stop(&s);
	if (s.rtp_sock >= 0) {
		close(s.rtp_sock);
	}
	close(sock);
	ESP_LOGI(TAG, "session closed");
}

static void rtsp_task(void *arg)
{
	(void)arg;

	int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	s_listen_sock = listen_sock;
	if (listen_sock < 0) {
		ESP_LOGE(TAG, "could not create the listening socket");
		s_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	int opt = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port        = htons(RTSP_SERVER_PORT),
	};

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listen_sock, 1) != 0) {
		ESP_LOGE(TAG, "bind/listen on port %d failed", RTSP_SERVER_PORT);
		close(listen_sock);
		s_listen_sock = -1;
		s_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_SERVER_PORT);

	while (s_running) {
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		int sock = accept(listen_sock, (struct sockaddr *)&peer, &peer_len);
		if (sock < 0) {
			if (!s_running) {
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		char ip[16];
		inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
		ESP_LOGI(TAG, "client connected from %s", ip);

		session_run(sock);
	}

	if (s_listen_sock >= 0) {
		close(s_listen_sock);
		s_listen_sock = -1;
	}
	s_task = NULL;
	vTaskDelete(NULL);
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t rtsp_server_start(void)
{
	ESP_RETURN_ON_FALSE(!s_task, ESP_ERR_INVALID_STATE, TAG, "already running");

	s_running = true;
	/* Generous stack: the SDP, request parsing and packetisation all use
	 * stack buffers, and lwIP calls are made from this task. */
	BaseType_t ok = xTaskCreate(rtsp_task, "rtsp", 6144, NULL, 5, &s_task);
	if (ok != pdPASS) {
		s_running = false;
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void rtsp_server_stop(void)
{
	if (!s_task) {
		return;
	}

	s_running = false;

	/* Closing the listening socket is what wakes the task out of accept();
	 * clearing s_running alone would leave it parked there for ever. */
	if (s_listen_sock >= 0) {
		shutdown(s_listen_sock, SHUT_RDWR);
		close(s_listen_sock);
		s_listen_sock = -1;
	}

	/* Wait for it to actually finish, so a later rtsp_server_start() does not
	 * trip over the old task still existing. */
	for (int i = 0; i < 100 && s_task; i++) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	if (s_task) {
		ESP_LOGW(TAG, "RTSP task did not exit");
	} else {
		ESP_LOGI(TAG, "RTSP server stopped");
	}
}

bool rtsp_server_is_playing(uint32_t *width, uint32_t *height)
{
	if (width)  { *width  = s_play_w; }
	if (height) { *height = s_play_h; }
	return s_playing;
}
