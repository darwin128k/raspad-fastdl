#ifndef FASTDL_HTTP_H
#define FASTDL_HTTP_H
#include <fastdl/config.h>
#include <authum/thread.h>
typedef struct fastdl_http fastdl_http_t;
/* Call start/stop on the owning (game) thread. Start opens sources and binds
 * synchronously; the worker owns all request state and never calls Authum.
 * Stop wakes and joins the worker before freeing any state. */
fastdl_http_t *fastdl_http_start(const fastdl_config_t *config,
                               const authum_threads_t *threads,
                               const char *module_dir, const char *game_dir,
                               char *error, unsigned error_size);
void fastdl_http_stop(fastdl_http_t *server);
/* Join without freeing, allowing the owner to drain final shutdown records. */
void fastdl_http_shutdown(fastdl_http_t *server);
authum_usize_t fastdl_http_memory_usage(const fastdl_http_t *server);
typedef void (*fastdl_http_log_fn)(const char *line, void *user);
/* Single consumer, on the owning thread. Never performs disk I/O itself. */
unsigned fastdl_http_drain_logs(fastdl_http_t *server, fastdl_http_log_fn sink,
                               void *user, unsigned limit);
#endif
