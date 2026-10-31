#include <fastdl/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fastdl_http
{
    authum_http_service_t service;
    authum_http_server_t *server;
};

static authum_bool_t
build_options(const fastdl_config_t *config, const authum_placeholder_service_t *placeholders,
              authum_http_options_t *options, authum_char_t *error, authum_usize_t error_size)
{
    authum_http_options_init(options);
    memcpy(options->bind, config->bind, sizeof options->bind);
    memcpy(options->allow_dirs, config->allow_dirs, sizeof options->allow_dirs);
    memcpy(options->allow_extensions, config->allow_extensions, sizeof options->allow_extensions);
    strcpy(options->root_extensions, "wad");
    memcpy(options->deny_prefixes, config->deny_prefixes, sizeof options->deny_prefixes);
    options->port = config->port;
    options->max_clients = config->max_clients;
    options->per_ip = config->per_ip;
    options->requests_per_second = config->requests_per_second;
    options->timeout_seconds = config->timeout_seconds;
    options->cpu = config->cpu;
    options->access_log = config->access_log ? authum_bool_true : authum_bool_false;
    options->bytes_per_second = config->bytes_per_second;
    options->max_file_bytes = config->max_file_bytes;

    for (authum_usize_t index = 0; index < config->source_count; ++index)
    {
        authum_char_t expanded[AUTHUM_HTTP_PATH_CAPACITY];
        if (!authum_placeholder_expand(placeholders, config->sources[index], expanded,
                                       sizeof expanded))
        {
            snprintf(error, error_size, "source[%u]: unknown placeholder or path too long",
                     index + 1);
            return authum_bool_false;
        }
        if (!authum_http_source_add(options, expanded))
        {
            snprintf(error, error_size, "source[%u]: invalid source path", index + 1);
            return authum_bool_false;
        }
    }
    return authum_bool_true;
}

fastdl_http_t *
fastdl_http_start(const fastdl_config_t *config, const authum_http_service_t *service,
                  const authum_placeholder_service_t *placeholders, authum_char_t *error,
                  authum_usize_t error_size)
{
    if (!service || !service->ops || !service->ops->open)
    {
        snprintf(error, error_size, "host has no HTTP service; update Authum");
        return NULL;
    }
    if (!placeholders || !placeholders->ops || !placeholders->ops->expand)
    {
        snprintf(error, error_size, "host has no placeholder service; update Authum");
        return NULL;
    }
    authum_http_options_t options;
    if (!build_options(config, placeholders, &options, error, error_size))
    {
        return NULL;
    }
    fastdl_http_t *http = calloc(1, sizeof *http);
    if (!http)
    {
        snprintf(error, error_size, "HTTP facade allocation failed");
        return NULL;
    }
    http->service = *service;
    http->server = authum_http_open(service, &options, error, error_size);
    if (!http->server)
    {
        free(http);
        return NULL;
    }
    return http;
}

void
fastdl_http_stop(fastdl_http_t *http)
{
    if (http)
    {
        authum_http_close(&http->service, http->server);
        free(http);
    }
}

void
fastdl_http_shutdown(fastdl_http_t *http)
{
    if (http)
    {
        authum_http_shutdown(&http->service, http->server);
    }
}

authum_usize_t
fastdl_http_memory_usage(const fastdl_http_t *http)
{
    return http ? authum_http_get_memory_usage(&http->service, http->server) + sizeof *http : 0;
}

authum_usize_t
fastdl_http_drain_logs(fastdl_http_t *http, fastdl_http_log_fn sink, authum_ptr user,
                       authum_usize_t limit)
{
    return http ? authum_http_drain(&http->service, http->server, sink, user, limit) : 0;
}
