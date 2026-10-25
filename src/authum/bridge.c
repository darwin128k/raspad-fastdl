#define _GNU_SOURCE
#include <fastdl/bridge.h>
#include <fastdl/http.h>
#include <authum/logger.h>
#include <authum/path.h>
#include <authum/event/server.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static authum_host_t g_host;
static authum_logger_t g_log;
static fastdl_http_t *g_http;
static authum_i32_t g_log_subscribed;
static authum_usize_t g_log_budget;
static authum_i64_t g_log_second;

static void
access_line(const authum_char_t *line, authum_ptr user)
{
    (void)user;
    authum_logger_info(&g_log, "%s", line);
}

static authum_event_result_t
access_frame(const authum_char_t *name, authum_ptr payload, authum_ptr user)
{
    (void)name;
    (void)payload;
    (void)user;
    authum_i64_t second = authum_clock_monotonic_ms(authum_host_clock(&g_host)) / 1000;
    if (second != g_log_second)
    {
        g_log_second = second;
        g_log_budget = 100;
    }
    authum_usize_t limit = g_log_budget < 8 ? g_log_budget : 8;
    g_log_budget -= fastdl_http_drain_logs(g_http, access_line, NULL, limit);
    return authum_event_result_continue;
}

void
fastdl_authum_stop(void)
{
    if (g_log_subscribed)
    {
        authum_events_unsubscribe(authum_host_events(&g_host), AUTHUM_EVENT_SERVER_FRAME,
                                  access_frame, NULL);
        g_log_subscribed = 0;
    }
    fastdl_http_shutdown(g_http);
    fastdl_http_drain_logs(g_http, access_line, NULL, 257);
    fastdl_http_stop(g_http);
    g_http = NULL;
    authum_logger_close(&g_log);
    authum_host_clear(&g_host);
}

authum_bool_t
fastdl_authum_start(authum_host_t *host)
{
    if (g_http)
    {
        return authum_bool_false;
    }
    authum_host_copy(&g_host, host);
    authum_logger_init(&g_log, &g_host);
    const authum_placeholder_service_t *placeholders = authum_host_placeholders(&g_host);
    if (!placeholders || !placeholders->ops || !AUTHUM_ABI_HAS(placeholders->ops, expand))
    {
        authum_logger_error(&g_log, "host has no placeholder service; update Authum");
        goto failed;
    }
    const authum_path_t *config_dir = authum_host_env_module_config(&g_host);
    authum_path_t config_path;
    authum_char_t error[512];
    if (!config_dir || !authum_path_join(&config_path, config_dir, "fastdl.cfg"))
    {
        authum_logger_error(&g_log, "host provides no module config directory");
        goto failed;
    }
    fastdl_config_t cfg;
    fastdl_config_defaults(&cfg);
    if (!fastdl_config_load(&cfg, authum_path_as_cstr(&config_path), error, sizeof error))
    {
        authum_logger_error(&g_log, "%s", error);
        goto failed;
    }
    authum_logger_info(&g_log, "config='%s', sources=%u, listen=%s:%d",
                       authum_path_as_cstr(&config_path), cfg.source_count, cfg.bind, cfg.port);
    for (authum_usize_t i = 0; i < cfg.source_count; i++)
    {
        authum_char_t expanded[AUTHUM_HTTP_PATH_CAPACITY];
        if (!authum_placeholder_expand(placeholders, cfg.sources[i], expanded, sizeof expanded))
        {
            authum_logger_error(&g_log, "source[%u]='%s': unknown placeholder or path too long",
                                i + 1, cfg.sources[i]);
            goto failed;
        }
        authum_logger_info(&g_log, "source[%u]='%s'", i + 1, expanded);
    }
    g_http = fastdl_http_start(&cfg, authum_host_http(&g_host), placeholders, error, sizeof error);
    if (!g_http)
    {
        authum_logger_error(&g_log, "HTTP startup: %s", error);
        goto failed;
    }
    if (cfg.access_log)
    {
        if (!authum_host_subscribe(&g_host, AUTHUM_EVENT_SERVER_FRAME, access_frame, NULL))
        {
            authum_logger_error(&g_log, "access_log requires Authum server-frame events");
            goto failed;
        }
        g_log_subscribed = 1;
        g_log_second = 0;
        g_log_budget = 100;
    }
    authum_logger_info(&g_log, "ready %s:%d, Authum worker (cpu=%d), clients=%d, buffers=%lu bytes",
                       cfg.bind, cfg.port, cfg.cpu, cfg.max_clients,
                       (unsigned long)fastdl_http_memory_usage(g_http));
    return authum_bool_true;
failed:
    authum_logger_error(&g_log, "FastDL startup failed");
    fastdl_authum_stop();
    return authum_bool_false;
}
