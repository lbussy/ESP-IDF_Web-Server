/**
 * @file http_server.cpp
 * @brief Native ESP-IDF HTTP server for ESP32.
 *
 * Threading model:
 * - esp_http_server runs its own internal task(s) and may invoke handlers
 *   concurrently.
 * - Handlers in this module only perform short, bounded work.
 * - A dedicated worker task is available for deferred actions.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "sdkconfig.h"

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_http_server.h"

#if CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
#include "esp_littlefs.h"
#endif
} // extern "C"

#include "http_pages.hpp"
#include "http_server.hpp"

static const char *TAG = "http_server";

#ifndef CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
#define CONFIG_HTTP_SERVER_ENABLE_LITTLEFS 0
#endif

#if CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
#ifndef CONFIG_HTTP_SERVER_LITTLEFS_MOUNT
#define CONFIG_HTTP_SERVER_LITTLEFS_MOUNT "/littlefs"
#endif

#ifndef CONFIG_HTTP_SERVER_LITTLEFS_LABEL
#define CONFIG_HTTP_SERVER_LITTLEFS_LABEL "littlefs"
#endif
#endif

namespace
{
    // -------------------------------------------------------------------------
    // Global state guarded by s_mutex.
    // -------------------------------------------------------------------------

    static SemaphoreHandle_t s_mutex = nullptr;
    static httpd_handle_t s_server = nullptr;

    static size_t s_max_open_sockets = 0U;
    static TaskHandle_t s_task = nullptr;
    static bool s_task_exit = false;

    enum class State
    {
        STOPPED,
        STARTING,
        RUNNING,
        STOPPING
    };

    static State s_state = State::STOPPED;

    static EventGroupHandle_t s_evt = nullptr;
    static constexpr EventBits_t READY_BIT = (1U << 0);

#if CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
    // -------------------------------------------------------------------------
    // LittleFS mount configuration.
    // -------------------------------------------------------------------------

    static bool s_fs_mounted = false;

    static constexpr const char *kDefaultFsBase = "/littlefs";
    static constexpr const char *kDefaultFsLabel = "littlefs";

    static const char *resolve_fs_base()
    {
        constexpr std::string_view cfg{CONFIG_HTTP_SERVER_LITTLEFS_MOUNT};

        if (cfg.empty())
        {
            ESP_LOGW(TAG, "CONFIG_HTTP_SERVER_LITTLEFS_MOUNT is empty, using default '%s'.",
                     kDefaultFsBase);
            return kDefaultFsBase;
        }

        if (cfg.front() != '/')
        {
            ESP_LOGW(TAG,
                     "CONFIG_HTTP_SERVER_LITTLEFS_MOUNT '%s' is missing leading '/', using '/%s'.",
                     CONFIG_HTTP_SERVER_LITTLEFS_MOUNT,
                     CONFIG_HTTP_SERVER_LITTLEFS_MOUNT);

            static std::string corrected;
            corrected = "/";
            corrected += CONFIG_HTTP_SERVER_LITTLEFS_MOUNT;
            return corrected.c_str();
        }

        return CONFIG_HTTP_SERVER_LITTLEFS_MOUNT;
    }

    static const char *resolve_fs_label()
    {
        constexpr std::string_view cfg{CONFIG_HTTP_SERVER_LITTLEFS_LABEL};

        if (cfg.empty())
        {
            ESP_LOGW(TAG, "CONFIG_HTTP_SERVER_LITTLEFS_LABEL is empty, using default '%s'.",
                     kDefaultFsLabel);
            return kDefaultFsLabel;
        }

        return CONFIG_HTTP_SERVER_LITTLEFS_LABEL;
    }

    static const char *kFsBase = resolve_fs_base();
    static const char *kFsLabel = resolve_fs_label();
#endif

    // -------------------------------------------------------------------------
    // Mutex helpers.
    // -------------------------------------------------------------------------

    static bool lock_mutex(TickType_t to = portMAX_DELAY)
    {
        if (s_mutex == nullptr)
        {
            return false;
        }
        return xSemaphoreTake(s_mutex, to) == pdTRUE;
    }

    static void unlock_mutex()
    {
        if (s_mutex != nullptr)
        {
            (void)xSemaphoreGive(s_mutex);
        }
    }

    static void notify_worker()
    {
        if (s_task != nullptr)
        {
            (void)xTaskNotifyGive(s_task);
        }
    }

    // -------------------------------------------------------------------------
    // Small utilities.
    // -------------------------------------------------------------------------

    static const char *status_for(int code)
    {
        switch (code)
        {
        case 200:
            return "200 OK";
        case 202:
            return "202 Accepted";
        case 204:
            return "204 No Content";
        case 302:
            return "302 Found";
        case 400:
            return "400 Bad Request";
        case 403:
            return "403 Forbidden";
        case 404:
            return "404 Not Found";
        case 405:
            return "405 Method Not Allowed";
        case 409:
            return "409 Conflict";
        case 413:
            return "413 Payload Too Large";
        case 415:
            return "415 Unsupported Media Type";
        default:
            return "500 Internal Server Error";
        }
    }

    static void set_no_cache_headers(httpd_req_t *req)
    {
        (void)httpd_resp_set_hdr(req, "Cache-Control",
                                 "no-cache, no-store, must-revalidate");
        (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
        (void)httpd_resp_set_hdr(req, "Expires", "0");
        (void)httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    }

    static void close_all_sessions_internal()
    {
        httpd_handle_t srv = nullptr;
        size_t max_socks = 0U;

        if (!lock_mutex())
        {
            return;
        }

        srv = s_server;
        max_socks = s_max_open_sockets;
        unlock_mutex();

        if (srv == nullptr || max_socks == 0U)
        {
            return;
        }

        std::vector<int> fds(max_socks, -1);
        size_t fds_len = max_socks;

        const esp_err_t rc = httpd_get_client_list(srv, &fds_len, fds.data());
        if (rc != ESP_OK)
        {
            return;
        }

        for (size_t i = 0U; i < fds_len; ++i)
        {
            if (fds[i] >= 0)
            {
                (void)httpd_sess_trigger_close(srv, fds[i]);
            }
        }
    }

    static esp_err_t send_text(httpd_req_t *req,
                               int code,
                               const char *ctype,
                               const char *body)
    {
        httpd_resp_set_status(req, status_for(code));
        httpd_resp_set_type(req, ctype);

        return httpd_resp_send(req,
                               body ? body : "",
                               body ? HTTPD_RESP_USE_STRLEN : 0);
    }

    static esp_err_t send_template(httpd_req_t *req,
                                   const char *tmpl,
                                   const char *ctype,
                                   bool no_cache = true)
    {
        if (req == nullptr || tmpl == nullptr || ctype == nullptr)
        {
            return ESP_FAIL;
        }

        if (no_cache)
        {
            set_no_cache_headers(req);
        }

        return send_text(req, 200, ctype, tmpl);
    }

#if CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
    // -------------------------------------------------------------------------
    // LittleFS file serving.
    // -------------------------------------------------------------------------

    static esp_err_t ensure_fs_mounted()
    {
        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if (s_fs_mounted)
        {
            unlock_mutex();
            return ESP_OK;
        }

        esp_vfs_littlefs_conf_t conf{};
        conf.base_path = kFsBase;
        conf.partition_label = kFsLabel;
        conf.format_if_mount_failed = true;
        conf.dont_mount = false;

        ESP_LOGI(TAG, "LittleFS mounting: label='%s' base='%s'.", kFsLabel, kFsBase);

        const esp_err_t rc = esp_vfs_littlefs_register(&conf);
        if (rc != ESP_OK)
        {
            unlock_mutex();
            ESP_LOGE(TAG, "LittleFS mount failed: %s.", esp_err_to_name(rc));
            return rc;
        }

        size_t total = 0U;
        size_t used = 0U;
        const esp_err_t info_rc = esp_littlefs_info(kFsLabel, &total, &used);
        if (info_rc == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "LittleFS total=%u used=%u.",
                     static_cast<unsigned>(total),
                     static_cast<unsigned>(used));
        }
        else
        {
            ESP_LOGW(TAG, "LittleFS info failed: %s.", esp_err_to_name(info_rc));
        }

        s_fs_mounted = true;
        unlock_mutex();
        return ESP_OK;
    }

    static bool has_dotdot(std::string_view uri)
    {
        return uri.find("..") != std::string_view::npos;
    }

    static bool ends_with(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() &&
               s.substr(s.size() - suffix.size()) == suffix;
    }

    static std::string content_type_for_path(std::string_view path_no_gz)
    {
        if (ends_with(path_no_gz, ".htm") || ends_with(path_no_gz, ".html"))
            return "text/html; charset=utf-8";
        if (ends_with(path_no_gz, ".css"))
            return "text/css; charset=utf-8";
        if (ends_with(path_no_gz, ".js"))
            return "application/javascript; charset=utf-8";
        if (ends_with(path_no_gz, ".json"))
            return "application/json; charset=utf-8";
        if (ends_with(path_no_gz, ".svg"))
            return "image/svg+xml";
        if (ends_with(path_no_gz, ".png"))
            return "image/png";
        if (ends_with(path_no_gz, ".jpg") || ends_with(path_no_gz, ".jpeg"))
            return "image/jpeg";
        if (ends_with(path_no_gz, ".gif"))
            return "image/gif";
        if (ends_with(path_no_gz, ".ico"))
            return "image/x-icon";
        if (ends_with(path_no_gz, ".woff2"))
            return "font/woff2";
        if (ends_with(path_no_gz, ".woff"))
            return "font/woff";
        if (ends_with(path_no_gz, ".ttf"))
            return "font/ttf";
        if (ends_with(path_no_gz, ".map"))
            return "application/json; charset=utf-8";

        return "text/plain; charset=utf-8";
    }

    static bool file_exists(const std::string &full_path)
    {
        FILE *f = std::fopen(full_path.c_str(), "rb");
        if (f == nullptr)
        {
            return false;
        }
        std::fclose(f);
        return true;
    }

    static bool resolve_fs_path(const char *uri,
                                std::string &out_full_path,
                                std::string &out_ctype,
                                bool &out_is_gz)
    {
        out_full_path.clear();
        out_ctype.clear();
        out_is_gz = false;

        if (uri == nullptr || uri[0] != '/')
        {
            return false;
        }

        std::string_view u(uri);
        if (has_dotdot(u))
        {
            return false;
        }

        std::string logical(uri);

        if (!logical.empty() && logical.back() == '/')
        {
            logical += "index.html";
        }
        if (logical == "/")
        {
            logical = "/index.html";
        }

        const bool requested_gz = ends_with(logical, ".gz");
        if (requested_gz && logical.size() > 3U)
        {
            logical.resize(logical.size() - 3U);
        }

        auto full = [&](const std::string &p) -> std::string
        {
            std::string fp;
            fp.reserve(std::strlen(kFsBase) + p.size() + 1U);
            fp += kFsBase;
            fp += p;
            return fp;
        };

        std::vector<std::string> candidates;
        candidates.reserve(2);
        candidates.push_back(logical);

        if (ends_with(logical, ".html"))
        {
            std::string alt = logical;
            alt.resize(alt.size() - 5U);
            alt += ".htm";
            if (alt != logical)
            {
                candidates.push_back(std::move(alt));
            }
        }
        else if (ends_with(logical, ".htm"))
        {
            std::string alt = logical;
            alt.resize(alt.size() - 4U);
            alt += ".html";
            if (alt != logical)
            {
                candidates.push_back(std::move(alt));
            }
        }

        for (const auto &cand : candidates)
        {
            const std::string gz = cand + ".gz";
            const std::string gz_full = full(gz);

            if (file_exists(gz_full))
            {
                out_full_path = gz_full;
                out_is_gz = true;
                out_ctype = content_type_for_path(cand);
                return true;
            }

            const std::string plain_full = full(cand);
            if (file_exists(plain_full))
            {
                out_full_path = plain_full;
                out_is_gz = false;
                out_ctype = content_type_for_path(cand);
                return true;
            }
        }

        return false;
    }

    static esp_err_t send_file_stream(httpd_req_t *req,
                                      const std::string &full_path,
                                      const std::string &ctype,
                                      bool is_gz)
    {
        FILE *f = std::fopen(full_path.c_str(), "rb");
        if (f == nullptr)
        {
            ESP_LOGW(TAG,
                     "File open failed: %s (errno=%d).",
                     full_path.c_str(),
                     errno);

            return send_text(req,
                             500,
                             "text/plain; charset=utf-8",
                             "File open failed\n");
        }

        httpd_resp_set_type(req, ctype.c_str());
        if (is_gz)
        {
            (void)httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }

        set_no_cache_headers(req);

        char buf[1024];
        while (true)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n > 0U)
            {
                const ssize_t send_len =
                    (n > static_cast<size_t>(std::numeric_limits<ssize_t>::max()))
                        ? std::numeric_limits<ssize_t>::max()
                        : static_cast<ssize_t>(n);

                const esp_err_t rc = httpd_resp_send_chunk(req, buf, send_len);
                if (rc != ESP_OK)
                {
                    std::fclose(f);
                    return rc;
                }
            }

            if (n < sizeof(buf))
            {
                break;
            }
        }

        std::fclose(f);
        return httpd_resp_send_chunk(req, nullptr, 0);
    }
#endif

    static esp_err_t try_serve_from_fs(httpd_req_t *req)
    {
#if !CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
        (void)req;
        return ESP_ERR_NOT_SUPPORTED;
#else
        if (req == nullptr || req->uri[0] == '\0')
        {
            return ESP_ERR_HTTPD_INVALID_REQ;
        }

        if (ensure_fs_mounted() != ESP_OK)
        {
            return ESP_ERR_NOT_SUPPORTED;
        }

        std::string full_path;
        std::string ctype;
        bool is_gz = false;

        if (!resolve_fs_path(req->uri, full_path, ctype, is_gz))
        {
            return ESP_ERR_NOT_FOUND;
        }

        return send_file_stream(req, full_path, ctype, is_gz);
#endif
    }

    // -------------------------------------------------------------------------
    // HTTP handlers.
    // -------------------------------------------------------------------------

    static esp_err_t handle_root(httpd_req_t *req)
    {
        const esp_err_t rc = try_serve_from_fs(req);
        if (rc == ESP_OK)
        {
            return ESP_OK;
        }

        if (rc != ESP_ERR_NOT_FOUND && rc != ESP_ERR_NOT_SUPPORTED)
        {
            return send_text(req,
                             500,
                             "text/plain; charset=utf-8",
                             "Internal file server error\n");
        }

        return send_template(req,
                             http_pages::kRoot,
                             "text/html; charset=utf-8");
    }

    static esp_err_t handle_favicon_ico(httpd_req_t *req)
    {
        const esp_err_t rc = try_serve_from_fs(req);
        if (rc == ESP_OK)
        {
            return ESP_OK;
        }

        if (rc != ESP_ERR_NOT_FOUND && rc != ESP_ERR_NOT_SUPPORTED)
        {
            return send_text(req,
                             500,
                             "text/plain; charset=utf-8",
                             "Internal file server error\n");
        }

        set_no_cache_headers(req);
        httpd_resp_set_type(req, "image/x-icon");

        return httpd_resp_send(
            req,
            reinterpret_cast<const char *>(http_pages::kFaviconIco),
            http_pages::kFaviconIcoSize);
    }

    // -------------------------------------------------------------------------
    // Server start/stop + URI registration.
    // -------------------------------------------------------------------------

    static esp_err_t register_uri_internal(const char *uri,
                                           httpd_method_t method,
                                           esp_err_t (*handler)(httpd_req_t *))
    {
        if (uri == nullptr || handler == nullptr)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if ((s_state != State::STARTING && s_state != State::RUNNING) ||
            s_server == nullptr)
        {
            unlock_mutex();
            return ESP_ERR_INVALID_STATE;
        }

        httpd_uri_t h{};
        h.uri = uri;
        h.method = method;
        h.handler = handler;
        h.user_ctx = nullptr;

        const esp_err_t rc = httpd_register_uri_handler(s_server, &h);
        unlock_mutex();
        return rc;
    }

    static void stop_server()
    {
        httpd_handle_t to_stop = nullptr;

        if (!lock_mutex())
        {
            return;
        }

        if (s_server == nullptr)
        {
            unlock_mutex();
            return;
        }

        to_stop = s_server;
        s_server = nullptr;
        unlock_mutex();

        const esp_err_t rc = httpd_stop(to_stop);
        if (rc != ESP_OK)
        {
            ESP_LOGW(TAG, "httpd_stop failed: %s.", esp_err_to_name(rc));
        }
    }

    static esp_err_t start_server()
    {
        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if (s_server != nullptr)
        {
            unlock_mutex();
            return ESP_OK;
        }

        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.uri_match_fn = httpd_uri_match_wildcard;
        cfg.max_uri_handlers = 40;

        s_max_open_sockets = static_cast<size_t>(cfg.max_open_sockets);

        const esp_err_t rc = httpd_start(&s_server, &cfg);
        if (rc != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_start failed: %s.", esp_err_to_name(rc));
            s_server = nullptr;
            unlock_mutex();
            return rc;
        }

        unlock_mutex();

#if CONFIG_HTTP_SERVER_ENABLE_LITTLEFS
        (void)ensure_fs_mounted();
#endif

        esp_err_t reg_rc = ESP_OK;

        reg_rc = register_uri_internal("/", HTTP_GET, handle_root);
        if (reg_rc != ESP_OK)
        {
            goto fail;
        }

        reg_rc = register_uri_internal("/index.html", HTTP_GET, handle_root);
        if (reg_rc != ESP_OK)
        {
            goto fail;
        }

        reg_rc = register_uri_internal("/index.htm", HTTP_GET, handle_root);
        if (reg_rc != ESP_OK)
        {
            goto fail;
        }

        reg_rc = register_uri_internal("/favicon.ico", HTTP_GET, handle_favicon_ico);
        if (reg_rc != ESP_OK)
        {
            goto fail;
        }

        return ESP_OK;

    fail:
        ESP_LOGE(TAG, "URI registration failed: %s.", esp_err_to_name(reg_rc));
        stop_server();
        return reg_rc;
    }

    // -------------------------------------------------------------------------
    // Worker task.
    // -------------------------------------------------------------------------

    static void http_srv_task(void *arg)
    {
        (void)arg;

        if (lock_mutex())
        {
            if (s_state == State::STARTING && s_evt != nullptr)
            {
                s_state = State::RUNNING;
                xEventGroupSetBits(s_evt, READY_BIT);
            }
            unlock_mutex();
        }

        while (true)
        {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            if (lock_mutex())
            {
                const bool exit_now = s_task_exit;
                unlock_mutex();
                if (exit_now)
                {
                    break;
                }
            }

            // Deferred work would run here.
        }

        if (lock_mutex())
        {
            if (s_evt != nullptr)
            {
                xEventGroupClearBits(s_evt, READY_BIT);
            }

            s_task = nullptr;
            unlock_mutex();
        }
        else
        {
            if (s_evt != nullptr)
            {
                xEventGroupClearBits(s_evt, READY_BIT);
            }
        }

        vTaskDelete(nullptr);
    }

    static bool ensure_mutex()
    {
        if (s_mutex == nullptr)
        {
            s_mutex = xSemaphoreCreateMutex();
            if (s_mutex == nullptr)
            {
                ESP_LOGE(TAG, "Failed to create mutex.");
                return false;
            }
        }

        if (s_evt == nullptr)
        {
            s_evt = xEventGroupCreate();
            if (s_evt == nullptr)
            {
                ESP_LOGE(TAG, "Failed to create event group.");
                vSemaphoreDelete(s_mutex);
                s_mutex = nullptr;
                return false;
            }
        }

        return true;
    }

    static bool start_worker_task()
    {
        if (!lock_mutex())
        {
            return false;
        }

        if (s_task != nullptr)
        {
            unlock_mutex();
            return true;
        }

        s_task_exit = false;
        unlock_mutex();

        static constexpr uint32_t kStackWords = 4096U;
        static constexpr UBaseType_t kPrio = 5;

        TaskHandle_t handle = nullptr;
        const BaseType_t ok =
            xTaskCreate(http_srv_task,
                        "http_srv",
                        kStackWords,
                        nullptr,
                        kPrio,
                        &handle);

        if (ok != pdPASS || handle == nullptr)
        {
            ESP_LOGE(TAG, "Failed to create http_srv task.");
            return false;
        }

        if (!lock_mutex())
        {
            vTaskDelete(handle);
            return false;
        }

        if (s_task != nullptr)
        {
            unlock_mutex();
            vTaskDelete(handle);
            return true;
        }

        s_task = handle;
        unlock_mutex();
        return true;
    }

    static bool stop_worker_task()
    {
        TaskHandle_t t = nullptr;

        if (!lock_mutex())
        {
            return false;
        }

        t = s_task;
        if (t == nullptr)
        {
            unlock_mutex();
            return true;
        }

        s_task_exit = true;
        unlock_mutex();

        notify_worker();

        for (int i = 0; i < 50; ++i)
        {
            if (lock_mutex(pdMS_TO_TICKS(50)))
            {
                const bool done = (s_task == nullptr);
                unlock_mutex();
                if (done)
                {
                    return true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        ESP_LOGW(TAG, "Worker task did not stop within timeout.");
        return false;
    }

    static void clear_deferred_state()
    {
        if (!lock_mutex())
        {
            return;
        }

        unlock_mutex();
    }
} // namespace

namespace http_srv
{
    void start()
    {
        if (!ensure_mutex())
        {
            return;
        }

        if (!lock_mutex())
        {
            return;
        }

        if (s_state == State::RUNNING ||
            s_state == State::STARTING ||
            s_state == State::STOPPING)
        {
            unlock_mutex();
            return;
        }

        s_state = State::STARTING;
        s_task_exit = false;
        xEventGroupClearBits(s_evt, READY_BIT);

        unlock_mutex();

        static constexpr int kMaxAttempts = 5;
        static constexpr TickType_t kInitialBackoff = pdMS_TO_TICKS(50);
        static constexpr TickType_t kWorkerReadyTimeout = pdMS_TO_TICKS(500);

        TickType_t backoff = kInitialBackoff;

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            const esp_err_t srv_ret = start_server();
            if (srv_ret != ESP_OK)
            {
                vTaskDelay(backoff);
                backoff = backoff + backoff;
                continue;
            }

            const bool task_ok = start_worker_task();
            if (!task_ok)
            {
                stop_server();
                vTaskDelay(backoff);
                backoff = backoff + backoff;
                continue;
            }

            const EventBits_t bits =
                xEventGroupWaitBits(s_evt,
                                    READY_BIT,
                                    pdFALSE,
                                    pdTRUE,
                                    kWorkerReadyTimeout);

            if ((bits & READY_BIT) != 0)
            {
                return;
            }

            (void)stop_worker_task();
            stop_server();

            vTaskDelay(backoff);
            backoff = backoff + backoff;
        }

        if (lock_mutex())
        {
            s_state = State::STOPPED;
            xEventGroupClearBits(s_evt, READY_BIT);
            unlock_mutex();
        }
    }

    void stop()
    {
        if (!ensure_mutex())
        {
            return;
        }

        TaskHandle_t worker = nullptr;

        if (!lock_mutex())
        {
            return;
        }

        if (s_state == State::STOPPED)
        {
            unlock_mutex();
            return;
        }

        s_state = State::STOPPING;
        xEventGroupClearBits(s_evt, READY_BIT);

        s_task_exit = true;
        worker = s_task;

        unlock_mutex();

        if (worker != nullptr)
        {
            notify_worker();
        }

        (void)stop_worker_task();
        clear_deferred_state();
        stop_server();

        if (lock_mutex(portMAX_DELAY))
        {
            s_state = State::STOPPED;
            xEventGroupClearBits(s_evt, READY_BIT);
            unlock_mutex();
        }
        else
        {
            ESP_LOGE(TAG, "Failed to finalize stop state.");
        }
    }

    bool is_running()
    {
        if (!ensure_mutex())
        {
            return false;
        }

        if (!lock_mutex())
        {
            return false;
        }

        const bool running = (s_state == State::RUNNING);
        unlock_mutex();
        return running;
    }

    esp_err_t wait_until_running(TickType_t timeout_ticks)
    {
        if (!ensure_mutex())
        {
            return ESP_FAIL;
        }

        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if (s_state == State::RUNNING)
        {
            unlock_mutex();
            return ESP_OK;
        }

        if (s_state == State::STOPPED || s_state == State::STOPPING)
        {
            unlock_mutex();
            return ESP_ERR_INVALID_STATE;
        }

        unlock_mutex();

        const EventBits_t bits =
            xEventGroupWaitBits(s_evt,
                                READY_BIT,
                                pdFALSE,
                                pdTRUE,
                                timeout_ticks);

        return (bits & READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    esp_err_t register_uri(const char *uri,
                           httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *))
    {
        if (uri == nullptr || handler == nullptr)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (!ensure_mutex())
        {
            return ESP_FAIL;
        }

        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if (s_state != State::RUNNING || s_server == nullptr)
        {
            unlock_mutex();
            return ESP_ERR_INVALID_STATE;
        }

        httpd_uri_t h{};
        h.uri = uri;
        h.method = method;
        h.handler = handler;
        h.user_ctx = nullptr;

        const esp_err_t rc = httpd_register_uri_handler(s_server, &h);
        unlock_mutex();
        return rc;
    }

    esp_err_t unregister_uri(const char *uri, httpd_method_t method)
    {
        if (uri == nullptr)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (!ensure_mutex())
        {
            return ESP_FAIL;
        }

        if (!lock_mutex())
        {
            return ESP_FAIL;
        }

        if (s_state != State::RUNNING || s_server == nullptr)
        {
            unlock_mutex();
            return ESP_ERR_INVALID_STATE;
        }

        const esp_err_t rc = httpd_unregister_uri_handler(s_server, uri, method);
        unlock_mutex();
        return rc;
    }

    void close_all_sessions()
    {
        if (!ensure_mutex())
        {
            return;
        }

        close_all_sessions_internal();
    }
} // namespace http_srv
