#ifndef FASTDL_HTTP_H
#define FASTDL_HTTP_H
#include <fastdl/config.h>
#include <authum/http.h>
#include <authum/placeholders.h>
typedef struct fastdl_http fastdl_http_t;
/* Converts module configuration/sources into Authum HTTP options.
 * All networking, parsing, limits and worker lifecycle live in Authum.
 * Call this facade on the owner/game thread. */
fastdl_http_t *
fastdl_http_start(const fastdl_config_t *config, const authum_http_service_t *service,
                  const authum_placeholder_service_t *placeholders, authum_char_t *error,
                  authum_usize_t error_size);
void
fastdl_http_stop(fastdl_http_t *server);
/* Join without freeing, allowing the owner to drain final shutdown records. */
void
fastdl_http_shutdown(fastdl_http_t *server);
authum_usize_t
fastdl_http_memory_usage(const fastdl_http_t *server);
typedef authum_http_log_fn fastdl_http_log_fn;
/* Single consumer, on the owning thread. Never performs disk I/O itself. */
authum_usize_t
fastdl_http_drain_logs(fastdl_http_t *server, fastdl_http_log_fn sink, void *user,
                       authum_usize_t limit);
#endif
