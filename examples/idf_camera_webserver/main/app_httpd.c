/*
 * Minimal HTTP server exposing the camera.
 *
 *   :80  GET /               the control page
 *   :80  GET /api/controls   list controls with ranges and current values
 *   :80  GET /api/control    set one control
 *   :80  GET /api/reset      restore defaults
 *   :80  GET /api/snapshot   save a still to the SD card
 *   :80  GET /api/photo      return a still as image/jpeg for download
 *   :80  GET /api/resolution choose the sensor mode for the preview
 *   :81  GET /stream         multipart/x-mixed-replace MJPEG stream
 *
 * The stream lives on a second port on purpose. esp_http_server processes
 * requests on a single task per instance, and the stream handler blocks for as
 * long as the client is connected, so sharing one instance would stall every
 * control request until streaming stopped.
 *
 * Only one stream client is served at a time: the P4 has a single camera
 * pipeline and one JPEG output buffer, so a second concurrent reader would
 * race over the same frame memory.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_camera.h"
#include "app_httpd.h"
#include "app_power.h"
#include "app_sensor.h"
#include "app_snapshot.h"
#include "rtsp_server.h"

static const char *TAG = "app_httpd";

#define STREAM_BOUNDARY "epdinkyframe"

/* The sensor mode the preview asks for. Changed at runtime via /api/resolution;
 * 720p is the default because it holds a solid 30 fps. */
#define MJPEG_DEFAULT_WIDTH  1280
#define MJPEG_DEFAULT_HEIGHT  720

static uint32_t s_preview_width  = MJPEG_DEFAULT_WIDTH;
static uint32_t s_preview_height = MJPEG_DEFAULT_HEIGHT;

/* How many consecutive encode failures end the stream. A few in a row means
 * something is genuinely wrong rather than one awkward frame. */
#define MAX_ENCODE_FAILURES 5

/* Enough for the control table plus the sensor's mode list. */
#define BODY_MAX 3072

static const char *STREAM_CONTENT_TYPE =
	"multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;
static const char *STREAM_PART_HEADER =
	"\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* Embedded by EMBED_FILES in the component CMakeLists. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server;
static httpd_handle_t s_stream_server;
static volatile bool  s_streaming;

/* ===========================================================================
 * Handlers
 * ========================================================================= */

static esp_err_t index_handler(httpd_req_t *req)
{
	app_power_note_activity();
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, (const char *)index_html_start,
	                       index_html_end - index_html_start - 1);
}

bool app_httpd_is_streaming(void)
{
	return s_streaming;
}

/* GET /api/controls -> every tunable control with its range and current value. */
static esp_err_t controls_get_handler(httpd_req_t *req)
{
	/* Deliberately does NOT count as activity. The page polls this to refresh
	 * the brightness reading, so treating it as a sign of life would let a
	 * forgotten browser tab hold the board awake for ever. Anything the user
	 * actually does - opening the stream, moving a slider, taking a photo -
	 * goes through a handler that does note activity.
	 */

	const app_camera_ctrl_t *ctrls = NULL;
	size_t count = 0;
	app_camera_get_controls(&ctrls, &count);

	uint32_t width = 0, height = 0;
	app_camera_get_size(&width, &height);

	/* Built by hand rather than with cJSON: the payload is small and fixed
	 * shape, and this keeps the example free of another dependency. */
	char  *body = malloc(BODY_MAX);
	if (!body) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		return httpd_resp_send(req, "out of memory", HTTPD_RESP_USE_STRLEN);
	}

	int n = snprintf(body, BODY_MAX,
	                 "{\"width\":%" PRIu32 ",\"height\":%" PRIu32
	                 ",\"luma\":%" PRId32 ",\"sd\":%s,",
	                 width, height, app_camera_get_measured_luma(),
	                 app_snapshot_sd_available() ? "true" : "false");

	esp_err_t   fail_err  = ESP_OK;
	const char *fail_step = app_power_last_failure(&fail_err);
	n += snprintf(body + n, BODY_MAX - n,
	              "\"wakes\":%" PRIu32 ",\"wake_reason\":\"%s\""
	              ",\"resume_fail\":\"%s\",\"resume_err\":\"%s\",",
	              app_power_wake_count(), app_power_last_wake_reason(),
	              fail_step, fail_step[0] ? esp_err_to_name(fail_err) : "");

	static const char *reset_names[] = {
		"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt",
		"wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse",
		"pwr_glitch", "cpu_lockup",
	};
	int rr = (int)esp_reset_reason();
	n += snprintf(body + n, BODY_MAX - n, "\"reset\":\"%s\",",
	              (rr >= 0 && rr < (int)(sizeof(reset_names) / sizeof(reset_names[0])))
	                  ? reset_names[rr] : "?");

	/* The resolutions the sensor can actually be put into. The page builds its
	 * chooser from this, so it can never offer a mode the sensor lacks. */
	const app_sensor_mode_t *modes = NULL;
	size_t mode_count = 0;
	app_sensor_get_modes(&modes, &mode_count);

	n += snprintf(body + n, BODY_MAX - n, "\"modes\":[");
	for (size_t i = 0; i < mode_count && n < BODY_MAX - 64; i++) {
		n += snprintf(body + n, BODY_MAX - n,
		              "%s{\"w\":%" PRIu32 ",\"h\":%" PRIu32 ",\"fps\":%" PRIu32 "}",
		              i ? "," : "", modes[i].width, modes[i].height, modes[i].fps);
	}
	n += snprintf(body + n, BODY_MAX - n, "],\"controls\":[");

	for (size_t i = 0; i < count && n < BODY_MAX - 200; i++) {
		n += snprintf(body + n, BODY_MAX - n,
		              "%s{\"key\":\"%s\",\"label\":\"%s\",\"id\":%" PRIu32
		              ",\"min\":%" PRId32 ",\"max\":%" PRId32 ",\"step\":%" PRId32
		              ",\"value\":%" PRId32 ",\"bool\":%s}",
		              i ? "," : "", ctrls[i].key, ctrls[i].label, ctrls[i].id,
		              ctrls[i].min, ctrls[i].max, ctrls[i].step, ctrls[i].value,
		              ctrls[i].is_bool ? "true" : "false");
	}
	n += snprintf(body + n, 2048 - n, "]}");

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	esp_err_t err = httpd_resp_send(req, body, n);
	free(body);
	return err;
}

/* GET /api/control?id=<id>&value=<v> -> set one control. */
static esp_err_t control_set_handler(httpd_req_t *req)
{
	app_power_note_activity();
	char query[96];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_send(req, "missing query string", HTTPD_RESP_USE_STRLEN);
	}

	char id_str[24], value_str[24];
	if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK ||
	    httpd_query_key_value(query, "value", value_str, sizeof(value_str)) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_send(req, "expected id and value", HTTPD_RESP_USE_STRLEN);
	}

	uint32_t id    = (uint32_t)strtoul(id_str, NULL, 10);
	int32_t  value = (int32_t)strtol(value_str, NULL, 10);

	esp_err_t err = app_camera_set_control(id, value);
	if (err == ESP_ERR_NOT_FOUND) {
		httpd_resp_set_status(req, "404 Not Found");
		return httpd_resp_send(req, "unknown control", HTTPD_RESP_USE_STRLEN);
	}
	if (err == ESP_ERR_INVALID_STATE) {
		httpd_resp_set_status(req, "409 Conflict");
		return httpd_resp_send(req, "turn auto exposure off first",
		                       HTTPD_RESP_USE_STRLEN);
	}
	if (err != ESP_OK) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		return httpd_resp_send(req, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
	}

	/* Report the value that was actually applied, since it may have been
	 * clamped to the control's range. */
	int32_t applied = value;
	app_camera_get_control(id, &applied);

	char body[64];
	int  len = snprintf(body, sizeof(body), "{\"id\":%" PRIu32 ",\"value\":%" PRId32 "}",
	                    id, applied);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, body, len);
}

/* GET /api/reset -> restore every control to its default. */
static esp_err_t reset_handler(httpd_req_t *req)
{
	app_power_note_activity();
	app_camera_reset_controls();
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/*
 * Both still-image endpoints share this: obtain a frame, then either write it
 * to the card or send it to the browser.
 *
 * Deliberately unavailable while RTSP is streaming: that stream owns the
 * camera and is encoding H.264, so there is no JPEG to hand over, and taking
 * the camera would interrupt a viewer.
 */
static bool still_precheck(httpd_req_t *req, bool as_json)
{
	uint32_t w, h;
	if (rtsp_server_is_playing(&w, &h)) {
		httpd_resp_set_status(req, "409 Conflict");
		if (as_json) {
			httpd_resp_set_type(req, "application/json");
		}
		httpd_resp_send(req, "{\"error\":\"an RTSP client is streaming\"}",
		                HTTPD_RESP_USE_STRLEN);
		return false;
	}
	return true;
}

static esp_err_t still_error(httpd_req_t *req, esp_err_t err)
{
	const char *msg = (err == ESP_ERR_TIMEOUT)
	                      ? "the stream did not deliver a frame"
	                      : esp_err_to_name(err);
	char body[128];
	int  len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);

	httpd_resp_set_status(req, "500 Internal Server Error");
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, body, len);
}

/* GET /api/snapshot -> save a still to the SD card. */
static esp_err_t snapshot_handler(httpd_req_t *req)
{
	app_power_note_activity();
	if (!still_precheck(req, true)) {
		return ESP_OK;
	}

	if (!app_snapshot_sd_available()) {
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_send(req, "{\"error\":\"no SD card mounted\"}",
		                       HTTPD_RESP_USE_STRLEN);
	}

	const uint8_t *data;
	size_t         len;
	esp_err_t err = app_snapshot_begin(s_streaming, &data, &len);
	if (err != ESP_OK) {
		return still_error(req, err);
	}

	char path[64] = {0};
	err = app_snapshot_write_sd(data, len, path, sizeof(path));
	app_snapshot_end();

	if (err != ESP_OK) {
		return still_error(req, err);
	}

	char body[128];
	int  n = snprintf(body, sizeof(body), "{\"file\":\"%s\",\"bytes\":%u}",
	                  path, (unsigned)len);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, body, n);
}

/*
 * GET /api/photo -> return a still as image/jpeg, for the browser to download.
 *
 * The client names the file, because it knows the real date and time and this
 * board's RTC may never have been set.
 */
static esp_err_t photo_handler(httpd_req_t *req)
{
	app_power_note_activity();
	if (!still_precheck(req, true)) {
		return ESP_OK;
	}

	const uint8_t *data;
	size_t         len;
	esp_err_t err = app_snapshot_begin(s_streaming, &data, &len);
	if (err != ESP_OK) {
		return still_error(req, err);
	}

	httpd_resp_set_type(req, "image/jpeg");
	httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"photo.jpg\"");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	err = httpd_resp_send(req, (const char *)data, len);

	app_snapshot_end();
	return err;
}

/*
 * GET /api/resolution?w=&h= -> choose the sensor mode for the preview.
 *
 * The size is only recorded here; it is applied when a stream next takes the
 * camera, because switching modes tears the capture down and cannot be done
 * underneath a running stream. The reply tells the page whether it needs to
 * reopen the stream for the change to show.
 */
static esp_err_t resolution_handler(httpd_req_t *req)
{
	app_power_note_activity();
	httpd_resp_set_type(req, "application/json");

	char query[64];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_send(req, "{\"error\":\"missing w and h\"}", HTTPD_RESP_USE_STRLEN);
	}

	char w_str[8], h_str[8];
	if (httpd_query_key_value(query, "w", w_str, sizeof(w_str)) != ESP_OK ||
	    httpd_query_key_value(query, "h", h_str, sizeof(h_str)) != ESP_OK) {
		httpd_resp_set_status(req, "400 Bad Request");
		return httpd_resp_send(req, "{\"error\":\"missing w and h\"}", HTTPD_RESP_USE_STRLEN);
	}

	uint32_t w = (uint32_t)atoi(w_str);
	uint32_t h = (uint32_t)atoi(h_str);

	if (!app_sensor_find_mode(w, h)) {
		httpd_resp_set_status(req, "404 Not Found");
		return httpd_resp_send(req, "{\"error\":\"the sensor has no such mode\"}",
		                       HTTPD_RESP_USE_STRLEN);
	}

	s_preview_width  = w;
	s_preview_height = h;

	/* When nothing is streaming, apply it now so stills use it straight away. */
	if (!s_streaming) {
		esp_err_t err = app_camera_set_mode(w, h);
		if (err != ESP_OK) {
			httpd_resp_set_status(req, "500 Internal Server Error");
			return httpd_resp_send(req, "{\"error\":\"mode switch failed\"}",
			                       HTTPD_RESP_USE_STRLEN);
		}
	}

	char body[96];
	int  n = snprintf(body, sizeof(body),
	                  "{\"w\":%" PRIu32 ",\"h\":%" PRIu32 ",\"restart\":%s}",
	                  w, h, s_streaming ? "true" : "false");
	return httpd_resp_send(req, body, n);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
	app_power_note_activity();
	if (s_streaming) {
		ESP_LOGW(TAG, "Refusing a second stream client");
		httpd_resp_set_status(req, "503 Service Unavailable");
		return httpd_resp_send(req, "A stream is already running", HTTPD_RESP_USE_STRLEN);
	}

	/* Take the camera. This displaces any RTSP session. */
	uint32_t token = app_camera_stream_acquire(APP_STREAM_MJPEG,
	                                           s_preview_width, s_preview_height);
	if (token == 0) {
		ESP_LOGW(TAG, "Could not take the camera for the MJPEG stream");
		httpd_resp_set_status(req, "503 Service Unavailable");
		return httpd_resp_send(req, "Camera busy", HTTPD_RESP_USE_STRLEN);
	}

	esp_err_t err = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
	if (err != ESP_OK) {
		app_camera_stream_release(token);
		return err;
	}
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

	s_streaming = true;
	ESP_LOGI(TAG, "Stream client connected");

	uint32_t frames = 0;
	int      encode_failures = 0;
	int64_t  started = esp_timer_get_time();

	while (true) {
		/* An RTSP client taking over ends this stream. */
		if (!app_camera_stream_is_owner(token)) {
			ESP_LOGI(TAG, "Camera taken by another consumer, ending MJPEG stream");
			break;
		}

		app_camera_frame_t frame;
		err = app_camera_capture_jpeg(0, 0, &frame);
		if (err != ESP_OK) {
			/* Drop the frame rather than the connection. A single encode can
			 * fail transiently, and killing the stream for it is a poor
			 * trade; only give up if it keeps happening. */
			if (++encode_failures > MAX_ENCODE_FAILURES) {
				ESP_LOGE(TAG, "Capture failed %d times, ending the stream: %s",
				         encode_failures, esp_err_to_name(err));
				break;
			}
			ESP_LOGW(TAG, "Dropped a frame: %s", esp_err_to_name(err));
			continue;
		}
		encode_failures = 0;

		/* Large enough for the boundary, content type and a 10-digit length. */
		char part[96];
		int header_len = snprintf(part, sizeof(part), STREAM_PART_HEADER,
		                          (unsigned)frame.len);

		err = httpd_resp_send_chunk(req, part, header_len);
		if (err == ESP_OK) {
			err = httpd_resp_send_chunk(req, (const char *)frame.data, frame.len);
		}

		/* Always hand the buffer back, including on a send failure. */
		app_camera_release();

		if (err != ESP_OK) {
			/* Normal when the browser closes the connection. */
			ESP_LOGI(TAG, "Stream client disconnected");
			break;
		}

		/* If a still was requested, this frame is the one it gets. Only a
		 * copy is taken, so the stream is held up by a memcpy rather than by
		 * an SD write or a socket send. */
		app_snapshot_offer_frame(frame.data, frame.len);

		frames++;
	}

	int64_t elapsed_us = esp_timer_get_time() - started;
	if (frames > 0 && elapsed_us > 0) {
		ESP_LOGI(TAG, "Streamed %" PRIu32 " frames in %.1f s (%.1f fps)",
		         frames, elapsed_us / 1e6, frames / (elapsed_us / 1e6));
	}

	s_streaming = false;
	app_camera_stream_release(token);
	httpd_resp_send_chunk(req, NULL, 0);
	return ESP_OK;
}

/* ===========================================================================
 * Lifecycle
 * ========================================================================= */

esp_err_t app_httpd_start(void)
{
	/* --- control server on port 80 --- */
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port      = 80;
	config.ctrl_port        = 32768;
	config.lru_purge_enable = true;
	config.max_open_sockets = 4;
	config.max_uri_handlers = 8;
	config.stack_size       = 8192;

	ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "failed to start the HTTP server");

	const httpd_uri_t index_uri = {
		.uri      = "/",
		.method   = HTTP_GET,
		.handler  = index_handler,
	};
	const httpd_uri_t controls_uri = {
		.uri      = "/api/controls",
		.method   = HTTP_GET,
		.handler  = controls_get_handler,
	};
	const httpd_uri_t control_uri = {
		.uri      = "/api/control",
		.method   = HTTP_GET,
		.handler  = control_set_handler,
	};
	const httpd_uri_t snapshot_uri = {
		.uri      = "/api/snapshot",
		.method   = HTTP_GET,
		.handler  = snapshot_handler,
	};
	const httpd_uri_t photo_uri = {
		.uri      = "/api/photo",
		.method   = HTTP_GET,
		.handler  = photo_handler,
	};
	const httpd_uri_t resolution_uri = {
		.uri      = "/api/resolution",
		.method   = HTTP_GET,
		.handler  = resolution_handler,
	};
	const httpd_uri_t reset_uri = {
		.uri      = "/api/reset",
		.method   = HTTP_GET,
		.handler  = reset_handler,
	};

	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_uri), TAG, "register / failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &controls_uri), TAG, "register /api/controls failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &control_uri), TAG, "register /api/control failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &reset_uri), TAG, "register /api/reset failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &snapshot_uri), TAG, "register /api/snapshot failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &photo_uri), TAG, "register /api/photo failed");
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &resolution_uri), TAG, "register /api/resolution failed");

	/* --- stream server on port 81, so it cannot block the controls --- */
	httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
	stream_config.server_port      = 81;
	stream_config.ctrl_port        = 32769;
	stream_config.lru_purge_enable = true;
	stream_config.max_open_sockets = 2;
	stream_config.stack_size       = 8192;

	ESP_RETURN_ON_ERROR(httpd_start(&s_stream_server, &stream_config), TAG,
	                    "failed to start the stream server");

	const httpd_uri_t stream_uri = {
		.uri      = "/stream",
		.method   = HTTP_GET,
		.handler  = stream_handler,
	};
	ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_stream_server, &stream_uri), TAG,
	                    "register /stream failed");

	ESP_LOGI(TAG, "HTTP server on port %d, MJPEG stream on port %d",
	         config.server_port, stream_config.server_port);
	return ESP_OK;
}

void app_httpd_stop(void)
{
	if (s_stream_server) {
		httpd_stop(s_stream_server);
		s_stream_server = NULL;
	}
	if (s_server) {
		httpd_stop(s_server);
		s_server = NULL;
	}
}
