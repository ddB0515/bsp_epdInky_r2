/*
 * The stage-2 provisioning page, served at "/" from a plain HTTP server on
 * the device's real station IP (no SoftAP, no captive-portal DNS hijack -
 * the device is already on the user's own network, so this is just an
 * ordinary page the setup screen tells them to browse to).
 *
 * The Home Assistant WebSocket host/port and long-lived access token -
 * ha_portal_wifi_page.h's stage-1 screen never asks for these. One submit
 * posts everything as one JSON body to POST /connect; Wi-Fi is untouched
 * here.
 *
 * On load, GET /current pre-fills every field from whatever is already in
 * NVS - this page is also what the button's "reconfigure" gesture (held
 * 5-20 s) drops back into without erasing anything first, specifically so
 * that a device already fully set up can be tweaked rather than re-entered
 * from scratch. The token field is the exception: /current reports only
 * whether it's already set, never the value, so it's left blank with a
 * placeholder saying so - leaving it blank on submit keeps the existing
 * token (see copy_json_str_keep_if_blank() in ha_portal.c); typing a new
 * value replaces it.
 *
 * Requires HA_PORTAL_WS_PORT_DEFAULT_STR to already be defined by the
 * includer (see ha_portal.c) - spliced into the markup below as the
 * fallback value used before /current has answered.
 *
 * Structurally identical to
 * examples/idf_epd_ha_firmware/main/portal/ha_portal_config_page.h, with the
 * MQTT-broker/dashboard-image/refresh-interval fields replaced by the Home
 * Assistant WebSocket host/port/token this firmware actually needs.
 */
#pragma once

static const char HA_PORTAL_CONFIG_HTML[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>epdInky Home Assistant Setup</title>\n"
"<style>\n"
"body{font:400 14px 'Calibri','Arial';margin:0}\n"
".div-container{display:flex;flex-direction:column;align-items:center;margin:24px}\n"
".form-group{display:flex;flex-direction:column;align-items:flex-start;margin-bottom:16px;width:100%;max-width:420px;position:relative}\n"
"label{font-size:15px;margin-bottom:4px}\n"
"small{color:#6c757d;margin-top:2px}\n"
"input{border-width:1px;border-radius:.5rem;font-size:16px;padding:6px;width:100%;box-sizing:border-box}\n"
"input:focus{outline:none;border:2px solid #33cc6b;border-radius:.5rem}\n"
".button{color:#fff;border:none;padding:8px 18px;font-size:16px;border-radius:5px;cursor:pointer}\n"
".button:disabled{opacity:.65}\n"
".button-success{background-color:#33cc6b}\n"
".button:hover:not([disabled]){filter:saturate(.8)}\n"
"h1{text-align:center;font-size:22px}\n"
"h2{font-size:16px;margin:8px 0;max-width:420px}\n"
".alert{padding:.6rem 1rem;margin-bottom:1rem;border:1px solid transparent;border-radius:.25rem;max-width:420px}\n"
".alert-primary{color:#383d41;background-color:#e2e3e5;border-color:#d6d8db}\n"
".alert-warning{color:#856404;background-color:#fff3cd;border-color:#ffeeba}\n"
"</style>\n"
"<script>\n"
"async function loadCurrent(){var r=await fetch('/current').catch(function(){return null});if(!r||r.status!=200)return;var j=await r.json();if(j.ws_host)document.getElementById('ws_host').value=j.ws_host;if(j.ws_port)document.getElementById('ws_port').value=j.ws_port;if(j.ws_token_set)document.getElementById('ws_token').placeholder='(already set \\u2014 leave blank to keep)'}\n"
"function submitForm(){hideAlert();var wsHost=document.getElementById('ws_host').value;if(!wsHost){displayAlert('Please enter your Home Assistant address');return}var body={ws_host:wsHost,ws_port:document.getElementById('ws_port').value,ws_token:document.getElementById('ws_token').value};var btn=document.getElementById('btnSave');btn.disabled=true;btn.textContent='Saving...';fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(function(r){return r.json()}).then(function(){displayAlert('Saved. Rebooting...','warning')}).catch(function(){btn.disabled=false;btn.textContent='Save and reboot';displayAlert('Failed to save. Please try again.')})}\n"
"function displayAlert(m){var b=document.getElementById('message');b.style.display='block';b.textContent=m;b.classList.add('alert-warning')}\n"
"function hideAlert(){document.getElementById('message').style.display='none'}\n"
"setTimeout(loadCurrent,200);\n"
"</script>\n"
"</head>\n"
"<body>\n"
"<h1>Finish setting up this epdInky display</h1>\n"
"<div class=\"div-container\">\n"
"<h2>Home Assistant</h2>\n"
"<div class=\"form-group\">\n"
"<label for=\"ws_host\">Home Assistant address (hostname or IP, no scheme)</label>\n"
"<input type=\"text\" id=\"ws_host\" placeholder=\"homeassistant.local\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<label for=\"ws_port\">Port</label>\n"
"<input type=\"number\" id=\"ws_port\" value=\"" HA_PORTAL_WS_PORT_DEFAULT_STR "\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<label for=\"ws_token\">Home Assistant long-lived access token</label>\n"
"<input type=\"password\" id=\"ws_token\" placeholder=\"<long-lived-access-token>\">\n"
"<small>Settings \\u2192 [your profile] \\u2192 Security \\u2192 Long-lived access tokens. Leave blank to keep the current token if one is already set.</small>\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<div class=\"alert alert-primary\" id=\"message\" role=\"alert\" style=\"display:none;\"></div>\n"
"</div>\n"
"<button id=\"btnSave\" class=\"button button-success\" onclick=\"submitForm()\">Save and reboot</button>\n"
"</div>\n"
"</body>\n"
"</html>\n";
