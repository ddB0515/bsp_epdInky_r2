/*
 * A captive-portal DNS responder: answers every A query with the SoftAP's own
 * IP, so a phone's "sign in to network" prompt (and a plain browser typing
 * any hostname) lands on the provisioning page instead of getting NXDOMAIN.
 *
 * Deliberately not a real resolver - there is exactly one answer this server
 * ever gives, for any question it is asked, which is what a captive portal is
 * supposed to do.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start the responder on UDP port 53.
 *
 * @param  ap_ip_be  The IP to answer with, as a big-endian uint32_t (the same
 *                    layout as esp_ip4_addr_t.addr / struct in_addr.s_addr).
 */
esp_err_t ha_dns_start(uint32_t ap_ip_be);

/** @brief Stop the responder and close its socket. */
void ha_dns_stop(void);

#ifdef __cplusplus
}
#endif
