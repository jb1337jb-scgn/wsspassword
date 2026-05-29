# ESP32-S3-N16R8 OCPP Wallbox Simulator - Basic AP+STA

Grundversion fuer den ESP32-S3-N16R8.

## WLAN

- Station WLAN: `internet` / `internet`
- Parallel AP: `Wallbox-Simulator` / `12345678`
- AP Webinterface: `http://192.168.4.1`

## Webinterface

Aenderbar und dauerhaft gespeichert:

- ChargeboxID
- BackendURL
- WSS Passwort

Default WSS Passwort: `12345678`

## GPIOs auf der 3V3-Seite

Eingaenge sind `INPUT_PULLUP`, aktiv gegen GND.

| Funktion | GPIO |
|---|---:|
| Plug Switch | 4 |
| Auth Button | 5 |
| Start Button | 6 |
| Stop Button | 7 |
| Fault Switch | 15 |
| Reset Button | 16 |
| Connect Button | 17 |
| Potentiometer | 3 |
| LED Available | 18 |
| LED Preparing | 8 |
| LED Charging | 9 |
| LED Faulted | 10 |
| Charge Enable | 11 |
| Debug LED | 12 |
| Relay Sim | 13 |
| Buzzer/Signal | 14 |
| Reserved | 46 |

## Interne RGB LED

NeoPixel auf GPIO 48:

- aus: keine Backendverbindung
- blau: WebSocket verbunden
- gruen: BootNotification accepted
