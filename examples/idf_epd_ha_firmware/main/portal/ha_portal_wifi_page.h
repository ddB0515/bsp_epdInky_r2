/*
 * The stage-1 provisioning page served at "/" from the SoftAP captive portal.
 *
 * Wi-Fi only: SSID (scan/select or typed) and password. Everything else this
 * firmware needs - the MQTT broker, the dashboard image URL/token, the
 * refresh interval - is collected later, by ha_portal_config_page.h, once
 * this device has joined a real network and has a DHCP address a browser can
 * actually reach. Splitting the two avoids asking for a broker address and a
 * Home Assistant token over a device's own open SoftAP, and keeps this first
 * screen to the one thing that has to happen before anything else can.
 *
 * Served plain, not gzipped: this is a one-time page over a direct SoftAP
 * hop, not a bandwidth-constrained path.
 */
#pragma once

static const char HA_PORTAL_WIFI_HTML[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>epdInky Wi-Fi Setup</title>\n"
"<style>\n"
"table{border-collapse:collapse;background:#fff;border-radius:4px;overflow:hidden;width:100%;margin:0 auto}\n"
"table td,table th{padding-left:8px;text-align:left}\n"
"table td{text-align:right}\n"
"table tbody tr{height:30px;border-bottom:1px solid #bbb;font-size:16px}\n"
"table tbody.hoverable tr:hover{background:#f2f2f2;cursor:pointer}\n"
".selected{background-color:#bbb}\n"
"body{font:400 14px 'Calibri','Arial';margin:0}\n"
".div-container{display:flex;flex-direction:column;align-items:center;margin:24px}\n"
".form-group{display:flex;flex-direction:column;align-items:flex-start;margin-bottom:16px;width:100%;max-width:420px;position:relative}\n"
"label{font-size:15px;margin-bottom:4px}\n"
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
"async function getInfo(){var infoBox=document.getElementById('scanning-info');var r=await fetch('/scan').catch(function(){return null});if(!r||r.status!=200){infoBox.textContent='Could not scan for networks. Enter your Wi-Fi name manually below.';return}var j=await r.json();infoBox.style.display='none';j.networks.forEach(function(n){appendWifiToTable(n.name,n.open,n.rssi)});document.getElementById('mac').textContent='Device MAC: '+j.mac}\n"
"function appendWifiToTable(name,open,rssi){var row=document.createElement('tr');row.onclick=function(){onClickItemTable(this)};var th=document.createElement('th');th.appendChild(document.createTextNode(name+(open?'':' (secured)')));row.appendChild(th);document.getElementById('table-networks').appendChild(row)}\n"
"function onClickItemTable(x){x.classList.add('selected');Array.from(x.parentNode.children).forEach(function(s){if(s!==x)s.classList.remove('selected')});var v=x.querySelector('th').firstChild.textContent.replace(/ \\(secured\\)$/,'');document.getElementById('ssid').value=v;document.getElementById('password').focus()}\n"
"function submitForm(){hideAlert();var ssid=document.getElementById('ssid').value;if(!ssid){displayAlert('Please enter or pick a Wi-Fi network');return}var body={ssid:ssid,pswd:document.getElementById('password').value};var btn=document.getElementById('btnSave');btn.disabled=true;btn.textContent='Saving...';fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(function(r){return r.json()}).then(function(r){displayAlert('Saved. Rebooting and connecting to \\''+r.ssid+'\\'...','warning')}).catch(function(){btn.disabled=false;btn.textContent='Save and reboot';displayAlert('Failed to save. Please try again.')})}\n"
"function displayAlert(m){var b=document.getElementById('message');b.style.display='block';b.textContent=m;b.classList.add('alert-warning')}\n"
"function hideAlert(){document.getElementById('message').style.display='none'}\n"
"setTimeout(getInfo,300);\n"
"</script>\n"
"</head>\n"
"<body>\n"
"<h1>Set up this epdInky display</h1>\n"
"<div class=\"div-container\">\n"
"<h2>Wi-Fi network</h2>\n"
"<table><tbody id=\"table-networks\" class=\"hoverable\"></tbody></table>\n"
"<p id=\"scanning-info\">Scanning networks, please wait...</p>\n"
"<p id=\"mac\" style=\"color:#6c757d;font-size:12px\"></p>\n"
"</div>\n"
"<div class=\"div-container\">\n"
"<div class=\"form-group\">\n"
"<label for=\"ssid\">Wi-Fi name (SSID)</label>\n"
"<input type=\"text\" id=\"ssid\" placeholder=\"Pick a network above or type one\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<label for=\"password\">Wi-Fi password</label>\n"
"<input type=\"password\" id=\"password\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<div class=\"alert alert-primary\" id=\"message\" role=\"alert\" style=\"display:none;\"></div>\n"
"</div>\n"
"<button id=\"btnSave\" class=\"button button-success\" onclick=\"submitForm()\">Save and reboot</button>\n"
"<p style=\"color:#6c757d;font-size:12px;max-width:420px;text-align:center\">Once connected, this device will show the address of a second setup page for MQTT and Home Assistant.</p>\n"
"</div>\n"
"</body>\n"
"</html>\n";
