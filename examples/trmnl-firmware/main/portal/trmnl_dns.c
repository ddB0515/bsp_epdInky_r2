#include "trmnl_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "lwip/sockets.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trmnl_dns";

#define DNS_PORT      53
#define DNS_MAX_PKT   512
#define DNS_TTL_S     60

static TaskHandle_t s_task;
static volatile bool s_running;

/*
 * Finds the end of the question section (a chain of length-prefixed labels
 * terminated by a zero byte, then QTYPE + QCLASS), so the response can copy
 * it verbatim rather than re-encoding the name. Returns the offset just past
 * QCLASS, or -1 if the packet is truncated.
 */
static int question_end(const uint8_t *buf, int len)
{
    int i = 12; /* past the fixed header */

    while (i < len) {
        uint8_t label_len = buf[i];
        if (label_len == 0) {
            i += 1;
            break;
        }
        if ((label_len & 0xC0) == 0xC0) {
            /* A compression pointer should not appear in a question, but a
             * malformed packet is just something to ignore, not crash on. */
            i += 2;
            break;
        }
        i += 1 + label_len;
    }
    i += 4; /* QTYPE + QCLASS */

    return (i <= len) ? i : -1;
}

static void dns_task(void *arg)
{
    const uint32_t ap_ip_be = (uint32_t)(uintptr_t)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* A receive timeout, not select()/poll(), so the loop below wakes up on
     * its own to notice trmnl_dns_stop() rather than blocking forever on a
     * quiet network. */
    struct timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "bind(:%d) failed: %d", DNS_PORT, errno);
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "answering every query on :%d", DNS_PORT);

    uint8_t req[DNS_MAX_PKT];
    uint8_t resp[DNS_MAX_PKT];

    while (s_running) {
        struct sockaddr_in from;
        socklen_t          from_len = sizeof(from);

        int n = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue; /* timeout, error, or a packet too short to be a query */
        }

        int qend = question_end(req, n);
        if (qend < 0) {
            continue;
        }

        /* Header: copy the request's ID, then overwrite everything else for
         * a plain, no-error, single-answer response. */
        memcpy(resp, req, 12);
        resp[2] = 0x81; /* QR=1 OPCODE=0 AA=1 TC=0 RD=1 */
        resp[3] = 0x80; /* RA=1 Z=0 RCODE=0 */
        resp[4] = 0x00; resp[5] = 0x01; /* QDCOUNT = 1 */
        resp[6] = 0x00; resp[7] = 0x01; /* ANCOUNT = 1 */
        resp[8] = 0x00; resp[9] = 0x00; /* NSCOUNT = 0 */
        resp[10] = 0x00; resp[11] = 0x00; /* ARCOUNT = 0 */

        memcpy(resp + 12, req + 12, qend - 12);
        int pos = qend;

        resp[pos++] = 0xC0; resp[pos++] = 0x0C; /* NAME: pointer to the question */
        resp[pos++] = 0x00; resp[pos++] = 0x01; /* TYPE = A */
        resp[pos++] = 0x00; resp[pos++] = 0x01; /* CLASS = IN */
        resp[pos++] = (DNS_TTL_S >> 24) & 0xFF;
        resp[pos++] = (DNS_TTL_S >> 16) & 0xFF;
        resp[pos++] = (DNS_TTL_S >> 8) & 0xFF;
        resp[pos++] = DNS_TTL_S & 0xFF;
        resp[pos++] = 0x00; resp[pos++] = 0x04; /* RDLENGTH = 4 */
        memcpy(resp + pos, &ap_ip_be, 4);
        pos += 4;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t trmnl_dns_start(uint32_t ap_ip_be)
{
    if (s_task) {
        return ESP_OK;
    }

    s_running = true;
    BaseType_t ok = xTaskCreate(dns_task, "trmnl_dns", 3072,
                                (void *)(uintptr_t)ap_ip_be, tskIDLE_PRIORITY + 1, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task    = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void trmnl_dns_stop(void)
{
    if (!s_task) {
        return;
    }

    s_running = false;

    /* dns_task() notices within one SO_RCVTIMEO period and clears s_task
     * itself on the way out; wait for that so the socket is really closed
     * before the caller tears down the AP it was bound through. */
    for (int waited_ms = 0; s_task && waited_ms < 2000; waited_ms += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
