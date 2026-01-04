# ESP-IDF Web-Server Basic Example

This is a minimal ESP-IDF application that demonstrates typical usage of the
`http_server` component in this repository.

---

## What it does

- Starts a Wi-Fi SoftAP with SSID `http-server`.
- Starts the `http_server` component.
- Registers a custom endpoint at `/api/ping` that returns `pong`.
- Optionally serves static files from LittleFS when enabled and configured.

---

## Build and flash (managed component)

This is the default workflow, where the example pulls `http_server` from the
ESP-IDF Component Manager.

From this directory:

```sh
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

Ensure the dependency is enabled in `main/idf_component.yml`:

```yaml
dependencies:
  lbussy/http_server: "^x.y.z"
  idf: ">=5.5"
```

---

## Local development build (using this repo)

This workflow builds the example against the local source tree instead of the
managed component. It is intended for development and testing of changes to
`http_server`.

### Steps

1. Comment out the managed dependency in `main/idf_component.yml`:

```yaml
dependencies:
  # lbussy/http_server: "^x.y.z"
  idf: ">=5.5"
```

2. Create a local component symlink:

```sh
mkdir -p components
ln -s ../../ components/http_server
```

3. Enable local development mode and reconfigure:

```sh
export HTTP_SERVER_LOCAL_DEV=1
idf.py fullclean
idf.py reconfigure
```

4. Build and flash:

```sh
idf.py build flash monitor
```

The example’s `REQUIRES http_server` declaration will now resolve to the local
source tree.

---

## LittleFS configuration

Static file serving from LittleFS is optional and controlled via Kconfig.

To configure:

```sh
idf.py menuconfig
```

Navigate to:

```
Filesystem mount point
  └─ Enable LittleFS support for HTTP server
```

When LittleFS support is enabled, the project **must** include the
`joltwallet/littlefs` component. This example does not automatically add that
dependency.

To add it using the ESP-IDF Component Manager:

```sh
idf.py add-dependency "joltwallet/littlefs^1"
```

After adding the dependency, reconfigure:

```sh
idf.py fullclean
idf.py reconfigure
```

---

## Test

- Connect a phone or laptop to the `http-server` Wi-Fi network.
- Browse to `http://192.168.4.1/`.
- Browse to `http://192.168.4.1/api/ping`.

---

## Notes

- The ESP-IDF native HTTP server runs its own internal task(s).
- URI handlers execute synchronously in the HTTP server context and must not
  block.
- When LittleFS is enabled, files are served from the configured mount point
  with automatic fallback to embedded resources.
