#ifndef FASTDL_CONFIG_H
#define FASTDL_CONFIG_H
#include <authum/http.h>

#define FASTDL_CONFIG_MAX_WARNINGS 8

typedef struct fastdl_config
{
    authum_char_t sources[AUTHUM_HTTP_MAX_SOURCES][AUTHUM_HTTP_PATH_CAPACITY];
    authum_usize_t source_count;
    authum_char_t bind[64];
    authum_char_t root[AUTHUM_HTTP_PATH_CAPACITY];
    authum_char_t allow_dirs[512];
    authum_char_t allow_extensions[512];
    authum_char_t deny_prefixes[512];
    /** Addresses or CIDR blocks of reverse proxies whose X-Forwarded-For is believed. */
    authum_char_t trusted_proxy[512];
    authum_i32_t port;
    authum_i32_t max_clients;
    authum_i32_t per_ip;
    authum_i32_t requests_per_second;
    authum_i32_t timeout_seconds;
    authum_i32_t cpu;
    /** 0 off, 1 one line per request plus a per-second refusal summary, 2 also connects. */
    authum_i32_t access_log;
    /** Connections per address that have not sent their request yet. */
    authum_i32_t per_ip_pending;
    authum_u64_t bytes_per_second;
    authum_u64_t max_file_bytes;
    /** Problems the config survived, for the caller to log as warnings. */
    authum_char_t warnings[FASTDL_CONFIG_MAX_WARNINGS][192];
    authum_usize_t warning_count;
} fastdl_config_t;

void
fastdl_config_defaults(fastdl_config_t *c);
authum_bool_t
fastdl_config_load(fastdl_config_t *c, const authum_char_t *path, authum_char_t *error,
                   authum_usize_t size);
#endif
