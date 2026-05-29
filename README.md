# ESP32-S3-N16R8 OCPP Wallbox Simulator - Real Station Behaviour

Based on the basic AP+STA firmware, adapted to behave closer to the provided real station communication.

## Network
- STA: internet / internet
- AP: Wallbox-Simulator / 12345678
- AP UI: http://192.168.4.1

## OCPP behavior
- BootNotification payload modeled after the real station
- Unique IDs like timestamp-random-counter
- Connector 1 StatusNotification on state changes
- Heartbeat uses interval from BootNotification.conf, default 300s
- StartTransaction.conf transactionId is parsed and reused for MeterValues and StopTransaction
- MeterValues are sent only while a transaction is active

## Web UI
Configurable:
- ChargeboxID
- BackendURL
- WSS password
- Basic Auth enabled
- Append ChargeboxID to URL

## Internal NeoPixel
- off: no backend
- blue: WebSocket connected
- green: BootNotification accepted
- red: fault

## TLS certificate

The Starfield Services Root Certificate Authority - G2 PEM is embedded in `src/main.cpp` as `ROOT_CA` and used for `wss://` connections via `ws.beginSSL(..., ROOT_CA)`.

## OCPP WebSocket subprotocol

The firmware now passes `ocpp1.6` via the WebSocketsClient protocol parameter instead of injecting `Sec-WebSocket-Protocol` as an extra header.

## Multiple ESP devices

AP SSID and default ChargeboxID are now generated from the ESP32 eFuse MAC suffix. Example: `Wallbox-SIM-A1B2C3` and `SIM_ESP32S3_A1B2C3`. If a ChargeboxID is changed in the web UI, the saved value is kept.


## v7 network checks

This version adds VPN-router friendly diagnostics:

- WiFi status: `connecting`, `connected`, `failed`
- DNS status: `not_checked`, `ok`, `failed`, `url_error`, `wifi_not_connected`
- NTP status: `not_checked`, `syncing`, `synced`, `failed`, `wifi_not_connected`
- WebSocket status: `disconnected`, `connecting`, `connected`
- OCPP status: `not_started`, `connecting`, `boot_sent`, `accepted`, `rejected`
- WSS protection: `wss://` connections are refused until NTP is synced.

For VPN-router operation, connect the ESP32 to the VPN router WiFi and make sure DNS, NTP, and TCP/443 are reachable through the tunnel.
