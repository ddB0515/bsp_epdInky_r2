/*
 * Idle detection and low-power sleep.
 *
 * When nothing is streaming and nobody has touched the web interface for a
 * while, there is no reason to keep the camera, the radio and the SD card
 * powered. This tears them all down and light-sleeps until the user button
 * (GPIO35) is pressed.
 *
 * WHY LIGHT SLEEP RATHER THAN DEEP SLEEP
 *
 * Deep sleep would take the ESP32-P4 itself lower, but on this board it would
 * save far less overall, because of where the radio is:
 *
 *   - Wi-Fi is an ESP32-C6 on the other side of an SDIO link, enabled by
 *     C6_CHIP_PU (GPIO54), which has a 10K pull-up (R43). To stop the C6
 *     drawing current, that pin has to be actively driven low.
 *   - Holding a single IO through deep sleep needs
 *     SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP, which the SoC header marks as
 *     "Supported only on ESP32P4 rev >= 3.0". This board is rev v1.0, so in
 *     deep sleep GPIO54 would float, the pull-up would win, and the C6 would
 *     boot and sit there consuming more than the sleeping P4 saves.
 *
 * Light sleep keeps the IO states, so the C6 can be held in reset for the whole
 * nap, and it keeps RAM, so waking is quick. On rev 3.0 silicon deep sleep
 * becomes the better option and only enter_sleep() needs changing.
 *
 * GPIO35 is the user button (SW4), active low with a 10K pull-up (R45), so the
 * wake trigger is a low level.
 */

#include <inttypes.h>

#include "driver/gpio.h"
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "app_httpd.h"
#include "app_power.h"
#include "rtsp_server.h"

static const char *TAG = "app_power";

#define WAKE_GPIO BSP_BUTTON_PIN   /* GPIO35, SW4 */

/*
 * Kept in RTC memory so it survives the software restart a failed resume
 * triggers. The USB console does not survive light sleep on the P4, so a
 * resume failure would otherwise be invisible - by the time the console works
 * again the board has rebooted and the message is gone.
 */
/* NOINIT rather than RTC_DATA: RTC_DATA is re-initialised on a software reset,
 * which is exactly the reset we need to survive. NOINIT keeps its contents, so
 * a magic value distinguishes real data from power-on garbage. */
#define FAIL_MAGIC 0x5A5AF00DU

RTC_NOINIT_ATTR static uint32_t  s_fail_magic;
RTC_NOINIT_ATTR static char      s_fail_step[24];
RTC_NOINIT_ATTR static esp_err_t s_fail_err;

static const app_power_hooks_t *s_hooks;
static uint32_t                 s_wake_count;
static esp_sleep_wakeup_cause_t s_last_wake_cause;
static esp_err_t                s_last_sleep_result;
static uint32_t                 s_idle_timeout_s = 60;
static volatile int64_t         s_last_activity_us;
static volatile bool            s_sleep_requested;
static TaskHandle_t             s_task;

/* ===========================================================================
 * Activity
 * ========================================================================= */

void app_power_note_activity(void)
{
	s_last_activity_us = esp_timer_get_time();
}

/* Anything that means a person or a client is actually using the board. */
static bool board_is_busy(void)
{
	uint32_t w, h;
	return app_httpd_is_streaming() || rtsp_server_is_playing(&w, &h);
}

uint32_t app_power_idle_seconds_left(void)
{
	if (board_is_busy()) {
		return 0;
	}
	int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
	int64_t left_s  = (int64_t)s_idle_timeout_s - (idle_us / 1000000);
	return (left_s > 0) ? (uint32_t)left_s : 0;
}

void app_power_sleep_now(void)
{
	s_sleep_requested = true;
}

/* ===========================================================================
 * Sleep
 * ========================================================================= */

/*
 * NOTE ON THE ESP32-C6 RADIO
 *
 * Holding C6_CHIP_PU (GPIO54) low over the nap would be the biggest single
 * saving, since the C6 is a whole second chip. It cannot be done: esp-hosted
 * treats a co-processor reboot as a fatal desynchronisation and calls
 * hosted_restart_host() -> esp_restart() to recover. Resetting the C6 around
 * sleep therefore reboots the P4 on every wake, which shows up as
 * esp_reset_reason() == ESP_RST_SW with no error of our own recorded.
 *
 * esp-hosted does have its own host power-save API
 * (esp_hosted_power_save_start()), but it is built for the slave waking the
 * host through an RTC GPIO, and only supports deep sleep. GPIO35 is the user
 * button and is not an RTC GPIO, so that path does not fit here either.
 *
 * What is left is still worthwhile: the Wi-Fi stack is stopped so the C6 has
 * no traffic to service, and the P4 - the camera, the ISP, the encoders and
 * the PSRAM - goes into light sleep.
 */

static esp_err_t enter_sleep(void)
{
	/* Level-triggered, because the button is active low and idle high. */
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << WAKE_GPIO,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,   /* R45 already does this */
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "wake pin config failed");

	/* If the button is already down, sleeping would end immediately. Wait for
	 * it to come back up so a long press does not turn into a no-op nap. */
	int waited_ms = 0;
	while (gpio_get_level(WAKE_GPIO) == BSP_BUTTON_ACTIVE_LEVEL && waited_ms < 5000) {
		vTaskDelay(pdMS_TO_TICKS(50));
		waited_ms += 50;
	}

	ESP_RETURN_ON_ERROR(gpio_wakeup_enable(WAKE_GPIO, GPIO_INTR_LOW_LEVEL), TAG,
	                    "gpio_wakeup_enable failed");
	ESP_RETURN_ON_ERROR(esp_sleep_enable_gpio_wakeup(), TAG,
	                    "esp_sleep_enable_gpio_wakeup failed");

	/* The USB serial console does not survive light sleep on the P4, so this
	 * is the last line that will appear until the board is reset. */
	ESP_LOGI(TAG, "Sleeping. Press the button on GPIO%d to wake.", (int)WAKE_GPIO);

	/* Let the log drain before the UART/USB clock stops. */
	vTaskDelay(pdMS_TO_TICKS(50));

	esp_err_t err = esp_light_sleep_start();

	/* The console is dead from here until a reset, so record what happened
	 * for /api/controls to report once the network is back. */
	s_last_sleep_result = err;
	s_last_wake_cause   = esp_sleep_get_wakeup_cause();
	s_wake_count++;

	gpio_wakeup_disable(WAKE_GPIO);
	esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

	return err;
}

static void suspend_and_sleep(void)
{
	ESP_LOGI(TAG, "Idle for %" PRIu32 " s - shutting down", s_idle_timeout_s);

	if (s_hooks && s_hooks->suspend) {
		s_hooks->suspend();
	}

	esp_err_t err = enter_sleep();

	int64_t slept_for = esp_timer_get_time();
	ESP_LOGI(TAG, "Woke up (%s)", err == ESP_OK ? "button" : esp_err_to_name(err));
	(void)slept_for;

	if (s_hooks && s_hooks->resume) {
		esp_err_t rerr = s_hooks->resume();
		if (rerr == ESP_OK) {
			s_fail_magic = 0;   /* resumed cleanly; drop any stale diagnosis */
		}
		if (rerr != ESP_OK) {
			if (s_fail_magic != FAIL_MAGIC) {
				app_power_record_failure("unknown", rerr);
			}
			ESP_LOGE(TAG, "Resume failed at %s (%s) - restarting",
			         s_fail_step, esp_err_to_name(rerr));
			esp_restart();
		}
	}

	app_power_note_activity();
}

static void power_task(void *arg)
{
	(void)arg;
	app_power_note_activity();

	uint32_t tick = 0;
	while (true) {
		vTaskDelay(pdMS_TO_TICKS(1000));

		/* Occasional breadcrumb so the countdown is visible in the log. */
		if (++tick % 15 == 0 && !board_is_busy()) {
			ESP_LOGI(TAG, "Idle, sleeping in %" PRIu32 " s",
			         app_power_idle_seconds_left());
		}

		if (board_is_busy()) {
			app_power_note_activity();
			continue;
		}

		bool timed_out = (esp_timer_get_time() - s_last_activity_us) >
		                 ((int64_t)s_idle_timeout_s * 1000000);

		if (s_sleep_requested || timed_out) {
			s_sleep_requested = false;
			suspend_and_sleep();
		}
	}
}

/*
 * Escape hatch: hold the wake button while the board boots to keep it awake.
 *
 * This matters more than it looks. Once the board is in light sleep the USB
 * serial port stops responding - the P4 does not support keeping USB alive in
 * light sleep (SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP is commented out in the
 * SoC header, IDF-6395) - so esptool cannot reset it into the bootloader and
 * the board cannot be reflashed until somebody presses the button. Holding the
 * button through boot gives a guaranteed way to get a flashable board back.
 */
static bool sleep_disabled_by_button(void)
{
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << WAKE_GPIO,
		.mode         = GPIO_MODE_INPUT,
		.pull_up_en   = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	if (gpio_config(&cfg) != ESP_OK) {
		return false;
	}
	return gpio_get_level(WAKE_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
}

esp_err_t app_power_start(const app_power_hooks_t *hooks, uint32_t idle_timeout_s)
{
	ESP_RETURN_ON_FALSE(hooks, ESP_ERR_INVALID_ARG, TAG, "hooks are required");
	ESP_RETURN_ON_FALSE(!s_task, ESP_ERR_INVALID_STATE, TAG, "already running");

	if (sleep_disabled_by_button()) {
		ESP_LOGW(TAG, "Button held at boot - idle sleep is disabled this session");
		return ESP_OK;
	}

	s_hooks          = hooks;
	s_idle_timeout_s = idle_timeout_s;
	app_power_note_activity();

	BaseType_t ok = xTaskCreate(power_task, "power", 4096, NULL, 4, &s_task);
	ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "could not start the power task");

	ESP_LOGI(TAG, "Idle sleep after %" PRIu32 " s, wake on GPIO%d",
	         idle_timeout_s, (int)WAKE_GPIO);
	return ESP_OK;
}

uint32_t app_power_wake_count(void)
{
	return s_wake_count;
}

const char *app_power_last_wake_reason(void)
{
	if (s_last_sleep_result != ESP_OK) {
		return "sleep rejected";
	}
	switch (s_last_wake_cause) {
	case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
	case ESP_SLEEP_WAKEUP_GPIO:      return "gpio";
	case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
	case ESP_SLEEP_WAKEUP_UART:      return "uart";
	default:                         return "other";
	}
}

void app_power_record_failure(const char *step, esp_err_t err)
{
	strlcpy(s_fail_step, step, sizeof(s_fail_step));
	s_fail_err   = err;
	s_fail_magic = FAIL_MAGIC;
}

const char *app_power_last_failure(esp_err_t *err)
{
	if (s_fail_magic != FAIL_MAGIC) {
		if (err) {
			*err = ESP_OK;
		}
		return "";
	}
	if (err) {
		*err = s_fail_err;
	}
	s_fail_step[sizeof(s_fail_step) - 1] = '\0';
	return s_fail_step;
}
