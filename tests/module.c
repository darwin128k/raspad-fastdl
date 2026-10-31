#define _GNU_SOURCE
#include <authum/module/api.h>
#include <fastdl/config.h>
#include <authum/host/threads.h>
#include <authum/host/http.h>
#include <authum/host/clock.h>
#include <authum/host/placeholders.h>
#include <authum/env.h>
#include <authum/path.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
static pthread_t owning_thread;
static unsigned access_lines;
static int wrong_log_thread;

static void
record_line(authum_ptr owner, authum_logger_level_t level, const authum_char_t *text)
{
    (void)owner;
    (void)level;
    if (!pthread_equal(pthread_self(), owning_thread))
    {
        wrong_log_thread = 1;
    }
    if (strstr(text, "http event="))
    {
        ++access_lines;
    }
}

static int
entries(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
    {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] != '.')
        {
            ++n;
        }
    }
    closedir(d);
    return n;
}

static void
pipe_signal(int n)
{
    (void)n;
}

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
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    const authum_module_api_t *api = dlsym(image, AUTHUM_MODULE_API_SYM);
    if (!api)
    {
        return 1;
    }
    authum_path_t path;
    if (!authum_path_set_cstr(&path, argv[2]))
    {
        return 1;
    }
    /* The host publishes the module's own folders; the test stands in for it.
     * The standard layout is <game>/addons/authum/modules/<name>, so the
     * game directory is four levels up. */
    authum_path_t config_dir;
    authum_path_t data_dir;
    char game_text[4096];
    if (snprintf(game_text, sizeof game_text, "%s/../../../..", argv[2]) >= (int)sizeof game_text)
    {
        return 1;
    }
    authum_path_t game_dir;
    if (!authum_path_join(&config_dir, &path, "configs") ||
        !authum_path_join(&data_dir, &path, "data") || !authum_path_set_cstr(&game_dir, game_text))
    {
        return 1;
    }
    authum_env_t env = {
        .game = &game_dir, .module = &path, .module_config = &config_dir, .module_data = &data_dir};
    authum_events_ops_t event_ops = {.subscribe = subscribe, .unsubscribe = unsubscribe};
    authum_events_t events = {.ops = &event_ops};
    owning_thread = pthread_self();
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
    int sentinel[2];
    if (pipe(sentinel))
    {
        return 1;
    }
    struct sigaction handler = {0}, saved;
    handler.sa_handler = pipe_signal;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGPIPE, &handler, NULL);
    cpu_set_t affinity_before, affinity_after;
    sched_getaffinity(0, sizeof affinity_before, &affinity_before);
    struct rlimit limit_before, limit_after;
    getrlimit(RLIMIT_CORE, &limit_before);
    int privileges = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    if (!getcwd(cwd, sizeof cwd))
    {
        return 1;
    }
    int fds = entries("/proc/self/fd"), threads = entries("/proc/self/task");
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
            return 1;
        }
        int active[2] = {-1, -1};
        if (started)
        {
            unsigned previous_lines = access_lines;
            if (entries("/proc/self/task") != threads + 1)
            {
                return 1;
            }
            if (cfg.cpu >= 0)
            {
                DIR *tasks = opendir("/proc/self/task");
                if (!tasks)
                {
                    return 1;
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
                        return 1;
                    }
                }
                closedir(tasks);
            }
            struct sockaddr_in addr = {.sin_family = AF_INET,
                                       .sin_port = htons((uint16_t)cfg.port)};
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            for (int k = 0; k < 2; k++)
            {
                active[k] = socket(AF_INET, SOCK_STREAM, 0);
                if (active[k] < 0 || connect(active[k], (struct sockaddr *)&addr, sizeof addr))
                {
                    return 1;
                }
                const char *request = k ? "GET /maps/large.bsp HTTP/1.0\r\n\r\n" : "GET /maps/";
                if (send(active[k], request, strlen(request), MSG_NOSIGNAL) < 0)
                {
                    return 1;
                }
            }
            usleep(10000);
            if (frame)
            {
                frame("authum.server.frame", NULL, frame_user);
            }
            /* Match the real loader: host joins workers BEFORE module stop. */
            authum_threads_host_stop_owner(&owner);
            api->stop();
            if (frame)
            {
                return 1;
            }
            if (cfg.access_log && access_lines <= previous_lines)
            {
                return 1;
            }
            if (wrong_log_thread)
            {
                return 1;
            }
            for (int k = 0; k < 2; k++)
            {
                close(active[k]);
            }
        }
        authum_http_host_drop_owner(&http_owner);
        authum_threads_host_drop_owner(&owner);
        int status;
        if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
        {
            return 1;
        }
        if (entries("/proc/self/fd") != fds || entries("/proc/self/task") != threads)
        {
            return 1;
        }
        if (fcntl(sentinel[0], F_GETFD) < 0 || fcntl(sentinel[1], F_GETFD) < 0)
        {
            return 1;
        }
        sigaction(SIGPIPE, NULL, &saved);
        if (saved.sa_handler != pipe_signal)
        {
            return 1;
        }
        if (!getcwd(after, sizeof after) || strcmp(cwd, after))
        {
            return 1;
        }
        sched_getaffinity(0, sizeof affinity_after, &affinity_after);
        if (!CPU_EQUAL(&affinity_before, &affinity_after))
        {
            return 1;
        }
        getrlimit(RLIMIT_CORE, &limit_after);
        if (limit_before.rlim_cur != limit_after.rlim_cur ||
            limit_before.rlim_max != limit_after.rlim_max)
        {
            return 1;
        }
        if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != privileges)
        {
            return 1;
        }
    }
    close(sentinel[0]);
    close(sentinel[1]);
    dlclose(image);
    return 0;
}
