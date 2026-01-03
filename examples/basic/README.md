# ESP-IDF Web-Server Basic Example

This is a minimal ESP-IDF application that demonstrates typical usage of the
`http_server` component in this repository.

## What it does

- Starts a Wi-Fi SoftAP with SSID `http-server`.
- Starts the `http_server` component.
- Registers a custom endpoint at `/api/ping` that returns `pong`.

## Build and flash

From this directory:

```sh
idf.py set-target esp32
idf.py build flash monitor
```

## Test

- Connect a phone or laptop to the `http-server` Wi-Fi network.
- Browse to `http://192.168.4.1/`.
- Browse to `http://192.168.4.1/api/ping`.

## Notes

The ESP-IDF native HTTP server runs its own internal task(s). This component
registers URI handlers using the standard `esp_http_server` API and serves
files from LittleFS when configured.
