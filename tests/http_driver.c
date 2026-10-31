#define _GNU_SOURCE
/* Test-only process wrapper around the exact same worker used by fastdl.so. */
#include <tests/platform.h>
#include <fastdl/http.h>
#include <authum/host/threads.h>
#include <authum/host/http.h>
#include <authum/host/placeholders.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static volatile sig_atomic_t stopped;

static void
on_signal(int n)
{
    (void)n;
    stopped = 1;
}

static void
access_line(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "%s\n", line);
}

int
main(int argc, char **argv)
{
    const char *config = NULL, *base = ".", *game_dir = NULL;
    int control = -1;
    unsigned control_port = 0;
    for (int i = 1; i < argc; i++)
    {
        if (i + 1 >= argc)
        {
            return 1;
        }
        if (!strcmp(argv[i], "--config"))
        {
            config = argv[++i];
        }
        else if (!strcmp(argv[i], "--base"))
        {
            base = argv[++i];
        }
        else if (!strcmp(argv[i], "--game-dir"))
        {
            game_dir = argv[++i];
        }
        else if (!strcmp(argv[i], "--control-fd"))
        {
            control = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--control-port"))
        {
            control_port = (unsigned)strtoul(argv[++i], NULL, 10);
        }
        else
        {
            return 1;
        }
    }
    if (!config)
    {
        return 1;
    }
    if (fastdl_socket_start())
    {
        return 1;
    }
    char module[4096], game[4096], fallback[4096], error[512];
    if (fastdl_real_path(base, module, sizeof module))
    {
        fastdl_socket_stop();
        return 1;
    }
    if (!game_dir)
    {
        if (snprintf(fallback, sizeof fallback,
                     "%s" FASTDL_PATH_SEPARATOR ".." FASTDL_PATH_SEPARATOR
                     ".." FASTDL_PATH_SEPARATOR ".." FASTDL_PATH_SEPARATOR "..",
                     module) >= (int)sizeof fallback)
        {
            fastdl_socket_stop();
            return 1;
        }
        game_dir = fallback;
    }
    if (fastdl_real_path(game_dir, game, sizeof game))
    {
        fastdl_socket_stop();
        return 1;
    }
    fastdl_config_t cfg;
    fastdl_config_defaults(&cfg);
    if (!fastdl_config_load(&cfg, config, error, sizeof error))
    {
        fprintf(stderr, "%s\n", error);
        fastdl_socket_stop();
        return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
#if defined(_WIN32)
    /* The harness stops the driver with CTRL_BREAK, which the CRT raises as
     * SIGBREAK: Windows has no SIGTERM delivery for another process. */
    signal(SIGBREAK, on_signal);
#endif
    authum_thread_owner_t owner;
    authum_threads_t threads;
    authum_threads_host_init(&owner, &threads);
    authum_http_owner_t http_owner;
    authum_http_service_t service;
    authum_http_host_init(&http_owner, &service);
    /* The same placeholder service the host binds: standard names of the
     * module being served. */
    char data_text[4096];
    if (snprintf(data_text, sizeof data_text, "%s/data", module) >= (int)sizeof data_text)
    {
        fastdl_socket_stop();
        return 1;
    }
    const authum_char_t *standard[authum_placeholder_standard_count] = {0};
    standard[authum_placeholder_game] = game;
    standard[authum_placeholder_module] = module;
    standard[authum_placeholder_data] = data_text;
    authum_placeholder_owner_t placeholder_owner;
    authum_placeholder_service_t placeholder_service;
    authum_placeholder_host_init(&placeholder_owner, standard, &placeholder_service);
    fastdl_http_t *http =
        fastdl_http_start(&cfg, &service, &placeholder_service, error, sizeof error);
    if (!http)
    {
        fprintf(stderr, "%s\n", error);
        fastdl_socket_stop();
        return 1;
    }
    fprintf(stderr, "buffers_bytes=%lu max_clients=%d\n",
            (unsigned long)fastdl_http_memory_usage(http), cfg.max_clients);
    /* The control channel: an inherited descriptor on POSIX, a loopback
     * connection on Windows, where a descriptor cannot cross processes. */
    fastdl_socket_t control_socket = FASTDL_SOCKET_INVALID;
    if (control_port)
    {
        control_socket = fastdl_socket_connect_loopback(control_port);
        if (control_socket == FASTDL_SOCKET_INVALID)
        {
            fastdl_http_stop(http);
            fastdl_socket_stop();
            return 1;
        }
    }
    else if (control >= 0)
    {
        control_socket = (fastdl_socket_t)control;
    }
    if (control_socket != FASTDL_SOCKET_INVALID &&
        fastdl_socket_send(control_socket, "READY", 5) != 5)
    {
        fastdl_http_stop(http);
        fastdl_socket_stop();
        return 1;
    }
    while (!stopped)
    {
        if (!getenv("FASTDL_TEST_HOLD_LOGS"))
        {
            fastdl_http_drain_logs(http, access_line, NULL, 16);
        }
        if (control_socket != FASTDL_SOCKET_INVALID)
        {
            if (fastdl_socket_wait_readable(control_socket, 50) > 0)
            {
                break;
            }
        }
        else
        {
            fastdl_sleep_ms(50);
        }
    }
    fastdl_http_shutdown(http);
    fastdl_http_drain_logs(http, access_line, NULL, 257);
    fastdl_http_stop(http);
    authum_placeholder_host_drop_owner(&placeholder_owner);
    authum_http_host_drop_owner(&http_owner);
    authum_threads_host_drop_owner(&owner);
    if (control_socket != FASTDL_SOCKET_INVALID)
    {
        fastdl_socket_close(control_socket);
    }
    fastdl_socket_stop();
    return 0;
}
