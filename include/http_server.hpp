#pragma once

/**
 * @file http_server.hpp
 * @brief Public interface for the embedded HTTP server module (native ESP-IDF).
 *
 * The server listens on port 80.
 *
 * The module runs a dedicated FreeRTOS task to process deferred actions
 * requested by HTTP handlers.
 *
 * Call stop() to stop the HTTP server and the worker task. stop() is idempotent
 * and thread-safe.
 */

extern "C"
{
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
} // extern "C"

namespace http_srv
{
    /**
     * @brief Start the HTTP server and worker task.
     *
     * This function is thread-safe and idempotent. It is safe to call from any
     * FreeRTOS task context.
     *
     * If the module is already running or is in the process of starting or
     * stopping, this function returns immediately.
     *
     * The module may perform bounded internal retries if startup fails due to a
     * transient condition. Use wait_until_running() to block until the worker
     * task has committed readiness.
     */
    void start();

    /**
     * @brief Stop the HTTP server and worker task.
     *
     * This function is thread-safe and idempotent. It is safe to call from any
     * FreeRTOS task context.
     *
     * If the module is not running, this function returns immediately.
     */
    void stop();

    /**
     * @brief Return true if the module is fully running.
     *
     * This returns true only when the HTTP server has started successfully and
     * the internal worker task has committed readiness.
     *
     * This function is thread-safe. It must not be called from an ISR.
     *
     * @return true if the module is running.
     * @return false otherwise.
     */
    bool is_running();

    /**
     * @brief Block until the HTTP server and worker task are running.
     *
     * This function waits until the HTTP server has started successfully and
     * the internal worker task has committed readiness. The wait ends when both
     * components are ready or when the specified timeout expires.
     *
     * This function is thread-safe and safe to call from any FreeRTOS task
     * context. It must not be called from an ISR.
     *
     * @param timeout_ticks Maximum time to wait, in FreeRTOS ticks.
     *        A value of 0 performs a non-blocking readiness check.
     *
     * @return ESP_OK if the server and worker task become running within the
     *         timeout.
     * @return ESP_ERR_TIMEOUT if the timeout expires before readiness is
     *         achieved.
     * @return ESP_ERR_INVALID_STATE if the module is stopped or stopping.
     * @return ESP_FAIL if the wait could not be performed.
     */
    esp_err_t wait_until_running(TickType_t timeout_ticks);

    /**
     * @brief Register a URI handler on the running server.
     *
     * This function is thread-safe and idempotent with respect to concurrent
     * stop() calls. It succeeds only when the module is fully running.
     *
     * @param uri URI string to register.
     * @param method HTTP method for the handler.
     * @param handler Handler function.
     *
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_STATE if the module is not running.
     * @return ESP_ERR_INVALID_ARG if uri or handler is null.
     * @return Other esp_err_t values as returned by ESP-IDF httpd registration.
     */
    esp_err_t register_uri(const char *uri,
                           httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *));

    /**
     * @brief Unregister a URI handler from the running server.
     *
     * This function is thread-safe and idempotent with respect to concurrent
     * stop() calls. It succeeds only when the module is fully running.
     *
     * @param uri URI string to unregister.
     * @param method HTTP method for the handler.
     *
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_STATE if the module is not running.
     * @return ESP_ERR_INVALID_ARG if uri is null.
     * @return Other esp_err_t values as returned by ESP-IDF httpd unregister.
     */
    esp_err_t unregister_uri(const char *uri, httpd_method_t method);

    /**
     * @brief Close all active HTTP sessions.
     *
     * Forces all currently connected clients to disconnect. This is useful
     * before switching Wi-Fi modes, applying provisioning changes, or
     * performing a reset.
     *
     * This function is thread-safe and idempotent. If the server is not
     * running, this function returns immediately.
     *
     * This function must not be called from an ISR.
     */
    void close_all_sessions();
} // namespace http_srv
