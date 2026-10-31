#define _GNU_SOURCE
#include <tests/platform.h>
#include <authum/module/api.h>
#include <fastdl/config.h>
#include <authum/host/threads.h>
#include <authum/host/http.h>
#include <authum/host/clock.h>
#include <authum/host/placeholders.h>
#include <authum/env.h>
#include <authum/path.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#    include <errno.h>
#    include <sched.h>
#    include <signal.h>
#    include <sys/prctl.h>
#    include <sys/resource.h>
#    include <sys/wait.h>
#endif
/** How many module log lines the failure report keeps. */
#define LOG_LINES 16
static fastdl_thread_id_t owning_thread;
static unsigned access_lines;
static int wrong_log_thread;
static char log_buffer[LOG_LINES][256];
static unsigned log_count;

static void
record_line(authum_ptr owner, authum_logger_level_t level, const authum_char_t *text)
{
    (void)owner;
    (void)level;
    if (!fastdl_thread_equal(fastdl_thread_self(), owning_thread))
    {
        wrong_log_thread = 1;
    }
    if (strstr(text, "http event="))
    {
        ++access_lines;
    }
    snprintf(log_buffer[log_count % LOG_LINES], sizeof log_buffer[0], "%s", text);
    ++log_count;
}

/** @brief Print the module's own log lines: they name the step that failed. */
static void
dump_log(void)
{
    unsigned first = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
    for (unsigned index = first; index < log_count; ++index)
    {
        fprintf(stderr, "module: %s\n", log_buffer[index % LOG_LINES]);
    }
}

/**
 * @brief Report a failed check with the module's log lines, and end the run.
 *
 * @param check Name of the check that failed.
 * @return The exit code of the run.
 */
static int
failed(const char *check)
{
    fprintf(stderr, "check failed: %s\n", check);
    dump_log();
    return 1;
}

#if !defined(_WIN32)
static void
pipe_signal(int n)
{
    (void)n;
}
#endif

static authum_event_handler_fn frame;
static authum_ptr frame_user;

static authum_bool_t
subscribe(authum_ptr owner, const authum_char_t *name, authum_event_handler_fn fn, authum_ptr user)
{
    (void)owner;
    (void)name;
    frame = fn;
    frame_user = user;
    return authum_bool_true;
}

static authum_bool_t
unsubscribe(authum_ptr owner, const authum_char_t *name, authum_event_handler_fn fn,
            authum_ptr user)
{
    (void)owner;
    (void)name;
    (void)fn;
    (void)user;
    frame = NULL;
    return authum_bool_true;
}

int
main(int argc, char **argv)
{
    if (argc != 4)
    {
        return 2;
    }
    /* Say what this platform cannot check before anything else: a skipped
     * check must never look like a passed one. */
    printf("platform=%s skipped_checks=%s substituted_checks=%s\n", fastdl_platform_name(),
           fastdl_skipped_checks(), fastdl_substituted_checks());
    fflush(stdout);
    if (fastdl_socket_start())
    {
        return 1;
    }
    fastdl_image_t image = fastdl_image_open(argv[1]);
    if (!image)
    {
        fprintf(stderr, "%s\n", fastdl_image_error());
        fastdl_socket_stop();
        return 1;
    }
    const authum_module_api_t *api = fastdl_image_symbol(image, AUTHUM_MODULE_API_SYM);
    if (!api)
    {
        fprintf(stderr, "module image exports no %s\n", AUTHUM_MODULE_API_SYM);
        fastdl_image_close(image);
        fastdl_socket_stop();
        return 1;
    }
    authum_path_t path;
    if (!authum_path_set_cstr(&path, argv[2]))
    {
        fastdl_image_close(image);
        fastdl_socket_stop();
        return 1;
    }
    /* The host publishes the module's own folders; the test stands in for it.
     * The standard layout is <game>/addons/authum/modules/<name>, so the
     * game directory is four levels up. */
    authum_path_t config_dir;
    authum_path_t data_dir;
    char game_text[4096];
    if (snprintf(game_text, sizeof game_text,
                 "%s" FASTDL_PATH_SEPARATOR ".." FASTDL_PATH_SEPARATOR ".." FASTDL_PATH_SEPARATOR
                 ".." FASTDL_PATH_SEPARATOR "..",
                 argv[2]) >= (int)sizeof game_text)
    {
        fastdl_image_close(image);
        fastdl_socket_stop();
        return 1;
    }
    authum_path_t game_dir;
    if (!authum_path_join(&config_dir, &path, "configs") ||
        !authum_path_join(&data_dir, &path, "data") || !authum_path_set_cstr(&game_dir, game_text))
    {
        fastdl_image_close(image);
        fastdl_socket_stop();
        return 1;
    }
    authum_env_t env = {
        .game = &game_dir, .module = &path, .module_config = &config_dir, .module_data = &data_dir};
    authum_events_ops_t event_ops = {.subscribe = subscribe, .unsubscribe = unsubscribe};
    authum_events_t events = {.ops = &event_ops};
    owning_thread = fastdl_thread_self();
    authum_logger_ops_t logger_ops = {.write = record_line};
    authum_logger_channel_t logger = {.ops = &logger_ops};
    authum_thread_owner_t owner;
    authum_threads_t thread_api;
    authum_http_owner_t http_owner;
    authum_http_service_t http_service;
    authum_placeholder_owner_t placeholder_owner;
    authum_placeholder_service_t placeholder_service;
    authum_placeholder_host_init_env(&placeholder_owner, &env, &placeholder_service);
    authum_host_t host = {.env = &env,
                          .events = &events,
                          .logger = &logger,
                          .threads = &thread_api,
                          .http = &http_service,
                          .clock = authum_clock_host(),
                          .placeholders = &placeholder_service};
    int expected = atoi(argv[3]);
    char cfg_path[4096], error[256], cwd[4096], after[4096];
    if (snprintf(cfg_path, sizeof cfg_path, "%s/configs/fastdl.cfg", argv[2]) >=
        (int)sizeof cfg_path)
    {
        return 1;
    }
    fastdl_config_t cfg;
    fastdl_config_defaults(&cfg);
    (void)fastdl_config_load(&cfg, cfg_path, error, sizeof error);
    fastdl_sentinel_t sentinel;
    if (fastdl_sentinel_open(&sentinel))
    {
        return 1;
    }
#if !defined(_WIN32)
    struct sigaction handler = {0}, saved;
    handler.sa_handler = pipe_signal;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGPIPE, &handler, NULL);
    cpu_set_t affinity_before, affinity_after;
    sched_getaffinity(0, sizeof affinity_before, &affinity_before);
    struct rlimit limit_before, limit_after;
    getrlimit(RLIMIT_CORE, &limit_before);
    int privileges = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
#endif
    if (fastdl_current_directory(cwd, sizeof cwd))
    {
        return 1;
    }
    int handles = fastdl_handle_count();
    int threads = fastdl_thread_count();
#if defined(_WIN32)
    int settled_handles = handles;
    int settled_threads = threads;
#endif
    for (int i = 0; i < 20; i++)
    {
        authum_threads_host_init(&owner, &thread_api);
        authum_http_host_init(&http_owner, &http_service);
        int started = api->start(&host) != 0;
        if (started != expected)
        {
            if (started)
            {
                api->stop();
            }
            return failed("module start result");
        }
        fastdl_socket_t active[2] = {FASTDL_SOCKET_INVALID, FASTDL_SOCKET_INVALID};
        if (started)
        {
            unsigned previous_lines = access_lines;
#if defined(_WIN32)
            /* The platform runtime starts helpers of its own: the worker has
             * to be there, not exactly one thread more than before. */
            if (fastdl_thread_count() <= threads)
            {
                return failed("worker thread missing");
            }
#else
            if (fastdl_thread_count() != threads + 1)
            {
                return failed("worker thread count");
            }
#endif
#if !defined(_WIN32)
            if (cfg.cpu >= 0)
            {
                DIR *tasks = opendir("/proc/self/task");
                if (!tasks)
                {
                    return failed("task directory");
                }
                struct dirent *entry;
                while ((entry = readdir(tasks)))
                {
                    pid_t tid = (pid_t)strtol(entry->d_name, NULL, 10);
                    if (tid <= 0 || tid == getpid())
                    {
                        continue;
                    }
                    cpu_set_t worker_cpu;
                    if (sched_getaffinity(tid, sizeof worker_cpu, &worker_cpu) ||
                        CPU_COUNT(&worker_cpu) != 1 || !CPU_ISSET(cfg.cpu, &worker_cpu))
                    {
                        closedir(tasks);
                        return failed("worker affinity");
                    }
                }
                closedir(tasks);
            }
#endif
            for (int k = 0; k < 2; k++)
            {
                active[k] = fastdl_socket_connect_loopback((unsigned)cfg.port);
                if (active[k] == FASTDL_SOCKET_INVALID)
                {
                    return failed("client connection");
                }
                const char *request = k ? "GET /maps/large.bsp HTTP/1.0\r\n\r\n" : "GET /maps/";
                if (fastdl_socket_send(active[k], request, (int)strlen(request)) < 0)
                {
                    return failed("client request");
                }
            }
            fastdl_sleep_ms(10);
            if (frame)
            {
                frame("authum.server.frame", NULL, frame_user);
            }
            /* Match the real loader: host joins workers BEFORE module stop. */
            authum_threads_host_stop_owner(&owner);
            api->stop();
            if (frame)
            {
                return failed("event subscription");
            }
            if (cfg.access_log && access_lines <= previous_lines)
            {
                return failed("access log");
            }
            if (wrong_log_thread)
            {
                return failed("log thread");
            }
            for (int k = 0; k < 2; k++)
            {
                fastdl_socket_close(active[k]);
            }
        }
        authum_http_host_drop_owner(&http_owner);
        authum_threads_host_drop_owner(&owner);
#if !defined(_WIN32)
        int status;
        if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
        {
            return failed("child process");
        }
#endif
#if defined(_WIN32)
        /* The loaded image keeps what it set up once: the counts must not
         * grow from cycle to cycle. */
        if (i == 0)
        {
            settled_handles = fastdl_handle_count();
            settled_threads = fastdl_thread_count();
            printf("platform=%s one_time_initialisation threads=%+d handles=%+d\n",
                   fastdl_platform_name(), settled_threads - threads, settled_handles - handles);
            fflush(stdout);
        }
        if (fastdl_handle_count() != settled_handles || fastdl_thread_count() != settled_threads)
        {
            return failed("descriptor or thread growth");
        }
#else
        if (fastdl_handle_count() != handles || fastdl_thread_count() != threads)
        {
            return failed("descriptor or thread leak");
        }
#endif
        if (!fastdl_sentinel_valid(&sentinel))
        {
            return failed("sentinel descriptors");
        }
#if !defined(_WIN32)
        sigaction(SIGPIPE, NULL, &saved);
        if (saved.sa_handler != pipe_signal)
        {
            return failed("signal handler");
        }
#endif
        if (fastdl_current_directory(after, sizeof after) || strcmp(cwd, after))
        {
            return failed("working directory");
        }
#if !defined(_WIN32)
        sched_getaffinity(0, sizeof affinity_after, &affinity_after);
        if (!CPU_EQUAL(&affinity_before, &affinity_after))
        {
            return failed("process affinity");
        }
        getrlimit(RLIMIT_CORE, &limit_after);
        if (limit_before.rlim_cur != limit_after.rlim_cur ||
            limit_before.rlim_max != limit_after.rlim_max)
        {
            return failed("core limit");
        }
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != privileges)
        {
            return failed("no new privileges");
        }
#endif
    }
    fastdl_sentinel_close(&sentinel);
    fastdl_image_close(image);
    fastdl_socket_stop();
    return 0;
}
