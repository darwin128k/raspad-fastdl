#ifndef FASTDL_CONFIG_H
#define FASTDL_CONFIG_H
typedef struct fastdl_config {
    char sources[16][4096];
    unsigned source_count;
    char bind[64], root[4096], allow_dirs[512], allow_extensions[512], deny_prefixes[512];
    int port, max_clients, per_ip, requests_per_second, timeout_seconds, cpu, access_log;
    unsigned long bytes_per_second, max_file_bytes;
} fastdl_config_t;
void fastdl_config_defaults(fastdl_config_t *c);
int fastdl_config_load(fastdl_config_t *c, const char *path, char *error, unsigned size);
int fastdl_config_expand(const char *source, const char *module, const char *game,
                         char *out, unsigned size);
#endif
