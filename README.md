# ESP-IDF Web Server

## Overview

This component provides a production-ready embedded HTTP server built on top of
the native ESP-IDF `esp_http_server` API. It wraps server lifecycle management,
optional filesystem-backed static file serving, and handler registration into a
small, thread-safe interface suitable for use in system services, provisioning
flows, and device configuration UIs.

The server listens on port 80 and can operate in two modes:

- Embedded-only mode, where all content is compiled into the firmware.
- LittleFS-backed mode, where static files are served from a LittleFS partition
  with automatic fallback to embedded resources.

The license for this component is MIT and is provided in the local `LICENSE`
file.

---

## Features

- Native ESP-IDF HTTP server integration.
- Thread-safe start and stop semantics.
- Deterministic worker task lifecycle.
- Optional static file serving from LittleFS.
- Automatic preference for precompressed `.gz` assets.
- Embedded fallback pages and favicon.
- Safe URI handler registration and removal.
- Explicit client session teardown support.

---

## Configuration

This component supports optional filesystem-backed content via LittleFS. All
filesystem support is controlled through Kconfig options.

### LittleFS dependency requirement

LittleFS support requires an external ESP-IDF component providing the LittleFS
driver. This HTTP server component does **not** vendor or bundle a filesystem
implementation.

When LittleFS support is enabled, you must add the following dependency to your
project:

- `joltwallet/littlefs`

### Relevant Kconfig options

- `CONFIG_HTTP_SERVER_ENABLE_LITTLEFS`
- `CONFIG_HTTP_SERVER_LITTLEFS_MOUNT`
- `CONFIG_HTTP_SERVER_LITTLEFS_LABEL`

### Adding the LittleFS component

```bash
idf.py add-dependency "joltwallet/littlefs^1"
```

---

## Common build and configuration errors

### LittleFS enabled but component missing

If LittleFS is enabled but the dependency is missing, the build will fail.
Add the dependency and run a full reconfigure.

```bash
idf.py fullclean
idf.py reconfigure
```

### Configuration options not visible

Ensure the component is included and reconfigure the project.

---

## Example application

A minimal example application is provided in `examples/basic`.

```bash
cd examples/basic
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

---

## License

This component is licensed under the MIT License.
