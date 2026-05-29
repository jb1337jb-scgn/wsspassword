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
