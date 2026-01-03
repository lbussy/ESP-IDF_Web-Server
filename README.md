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

## Asynchronous vs synchronous behavior

The HTTP server is asynchronous at the I/O and dispatch level, following the
design of ESP-IDF’s `esp_http_server`, but request handlers themselves execute
synchronously.

### What is asynchronous

- TCP socket accept, read, and write operations.
- HTTP parsing and request dispatch.
- Concurrent handling of multiple client connections.
- Streaming responses using chunked transfer encoding.

These behaviors are implemented by ESP-IDF internal FreeRTOS tasks owned by the
HTTP server.

### What is synchronous

- URI handler functions run synchronously in the context of the HTTP server
  task.
- Any blocking or long-running work performed inside a handler will block the
  server and delay processing of other requests.

### Worker task model

This component provides a dedicated FreeRTOS worker task intended for deferred
or long-running work. URI handlers should perform only short, bounded actions,
such as input validation or state updates, then signal the worker task to perform
any heavy processing asynchronously.

This design follows ESP-IDF best practices and avoids blocking the HTTP server
task.

---

## Configuration

This component supports optional filesystem-backed content via LittleFS. All
filesystem support is controlled through Kconfig options.

### Enabling or disabling LittleFS

LittleFS support can be enabled or disabled at build time:

- When enabled, the server will attempt to mount LittleFS and serve static files
  from it.
- When disabled, the server will not depend on LittleFS and will serve embedded
  resources only.

Kconfig option:

- `HTTP_SERVER_ENABLE_LITTLEFS`

When this option is disabled, all LittleFS-related configuration options are
hidden and the component does not depend on the LittleFS driver.

### LittleFS configuration options

When LittleFS support is enabled, the following options are available:

- `HTTP_SERVER_LITTLEFS_MOUNT`  
  Internal mount point used by the HTTP server. Must start with `/`.

- `HTTP_SERVER_LITTLEFS_LABEL`  
  Partition label passed to the LittleFS driver. Do not include a leading `/`.

These options control how and where the filesystem is mounted. They must match
the partition table configuration used by the application.

---

## API overview

The `http_srv` namespace exposes a small control surface for managing the HTTP
server and its internal worker task. All functions are safe to call from normal
FreeRTOS task context.

### Server lifecycle

#### Starting the server

`http_srv::start()` initializes internal synchronization primitives, starts the
ESP-IDF HTTP server, conditionally mounts the filesystem, registers built-in
handlers, and launches the worker task. The call is idempotent and returns
immediately if the server is already running or transitioning state.

```cpp
#include "http_server.hpp"

void app_main(void)
{
    http_srv::start();
}
```

#### Stopping the server

`http_srv::stop()` stops the HTTP server, disconnects all active clients, signals
the worker task to exit, and transitions the module into a stopped state.

```cpp
http_srv::stop();
```

### Readiness and state

#### Checking running state

`http_srv::is_running()` returns true only when the server and worker task are
fully running.

```cpp
if (http_srv::is_running())
{
    // Server is ready.
}
```

#### Waiting for readiness

`http_srv::wait_until_running()` blocks until the server becomes ready or the
specified timeout expires.

```cpp
esp_err_t rc =
    http_srv::wait_until_running(pdMS_TO_TICKS(1000));
```

### URI handler management

#### Registering a handler

Handlers may be registered only while the server is running.

```cpp
static esp_err_t ping_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "pong\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

http_srv::register_uri("/api/ping",
                       HTTP_GET,
                       ping_handler);
```

#### Unregistering a handler

```cpp
http_srv::unregister_uri("/api/ping", HTTP_GET);
```

### Client session control

#### Closing all sessions

```cpp
http_srv::close_all_sessions();
```

---

## Filesystem behavior

When LittleFS support is enabled:

- Static files are served from the configured LittleFS mount point.
- If a `.gz` version of a file exists, it is always preferred and served with
  `Content-Encoding: gzip`, regardless of client headers.
- If a requested file does not exist, the server falls back to embedded
  resources.
- `.html` and `.htm` extensions are treated as interchangeable.

When LittleFS support is disabled:

- All filesystem operations are disabled.
- Only embedded resources are served.

---

## Threading model

- ESP-IDF HTTP server runs in its own internal task.
- URI handlers perform short, bounded work only.
- A dedicated worker task exists for deferred operations.
- All shared state is protected by a mutex.

---

## Example application

A minimal example application is provided in `examples/basic`.

### What the example demonstrates

- Starting the HTTP server.
- Waiting for readiness.
- Registering a custom API endpoint.
- Serving both embedded content and filesystem-backed content when enabled.

The example starts the device in Wi-Fi SoftAP mode and registers
`GET /api/ping`, which returns `pong`.

### Running the example

```bash
cd examples/basic
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

After flashing, connect to the access point created by the device and open:

- `http://192.168.4.1/`
- `http://192.168.4.1/api/ping`

---

## License

This component is licensed under the MIT License. See the included `LICENSE`
file for full license text.
