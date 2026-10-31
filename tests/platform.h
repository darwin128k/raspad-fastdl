/**
 * @file platform.h
 * @brief Test-harness platform seam: sockets, module images, handles, threads.
 *
 * The module names no platform. The harness does, and only here: both drivers
 * include this header, so the Windows and Linux builds share one source.
 */
#ifndef FASTDL_TEST_PLATFORM_H
#define FASTDL_TEST_PLATFORM_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <windows.h>
#    include <tlhelp32.h>
#else
#    include <arpa/inet.h>
#    include <dirent.h>
#    include <dlfcn.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <pthread.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

#if defined(_WIN32)
/** Native socket handle of this platform. */
typedef SOCKET fastdl_socket_t;
/** Native module image handle of this platform. */
typedef HMODULE fastdl_image_t;
/** Native thread identity of this platform. */
typedef DWORD fastdl_thread_id_t;

/** Socket value that means "no socket". */
#    define FASTDL_SOCKET_INVALID INVALID_SOCKET
/** Image value that means "no image". */
#    define FASTDL_IMAGE_INVALID NULL
/** Separator the platform's own file APIs expect. */
#    define FASTDL_PATH_SEPARATOR "\\"
/** Nonzero when the build runs on Linux, where the extra checks are possible. */
#    define FASTDL_TEST_LINUX 0
#else
/** Native socket handle of this platform. */
typedef int fastdl_socket_t;
/** Native module image handle of this platform. */
typedef void *fastdl_image_t;
/** Native thread identity of this platform. */
typedef pthread_t fastdl_thread_id_t;

/** Socket value that means "no socket". */
#    define FASTDL_SOCKET_INVALID (-1)
/** Image value that means "no image". */
#    define FASTDL_IMAGE_INVALID NULL
/** Separator the platform's own file APIs expect. */
#    define FASTDL_PATH_SEPARATOR "/"
/** Nonzero when the build runs on Linux, where the extra checks are possible. */
#    define FASTDL_TEST_LINUX 1
#endif

/**
 * @brief Start the platform socket stack.
 *
 * @return 0 when the stack is ready, -1 when it is not.
 */
static inline int
fastdl_socket_start(void)
{
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

/** @brief Stop the platform socket stack. */
static inline void
fastdl_socket_stop(void)
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

/**
 * @brief Wait until a socket becomes readable.
 *
 * @param socket     Socket to watch.
 * @param timeout_ms Longest wait in milliseconds.
 * @return 1 when readable, 0 on timeout, -1 on failure.
 */
static inline int
fastdl_socket_wait_readable(fastdl_socket_t socket, int timeout_ms)
{
#if defined(_WIN32)
    WSAPOLLFD item;
    int ready;

    item.fd = socket;
    item.events = POLLRDNORM;
    item.revents = 0;
    ready = WSAPoll(&item, 1, timeout_ms);
#else
    struct pollfd item;
    int ready;

    item.fd = socket;
    item.events = POLLIN;
    item.revents = 0;
    ready = poll(&item, 1, timeout_ms);
#endif
    if (ready < 0)
    {
        return -1;
    }
    return ready > 0 ? 1 : 0;
}

/**
 * @brief Send bytes on a socket without raising SIGPIPE.
 *
 * @param socket Socket to write to.
 * @param data   Bytes to write.
 * @param size   Number of bytes to write.
 * @return Bytes written, or -1 on failure.
 */
static inline int
fastdl_socket_send(fastdl_socket_t socket, const void *data, int size)
{
#if defined(_WIN32)
    return send(socket, (const char *)data, size, 0);
#else
#    ifdef MSG_NOSIGNAL
    return (int)send(socket, data, (size_t)size, MSG_NOSIGNAL);
#    else
    return (int)send(socket, data, (size_t)size, 0);
#    endif
#endif
}

/** @brief Close a socket. */
static inline void
fastdl_socket_close(fastdl_socket_t socket)
{
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

/**
 * @brief Connect to a loopback TCP port.
 *
 * @param port TCP port on 127.0.0.1.
 * @return The connected socket, or ::FASTDL_SOCKET_INVALID on failure.
 */
static inline fastdl_socket_t
fastdl_socket_connect_loopback(unsigned port)
{
    struct sockaddr_in address;
    fastdl_socket_t handle;

    handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == FASTDL_SOCKET_INVALID)
    {
        return FASTDL_SOCKET_INVALID;
    }
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(handle, (const struct sockaddr *)&address, sizeof address) != 0)
    {
        fastdl_socket_close(handle);
        return FASTDL_SOCKET_INVALID;
    }
    return handle;
}

/**
 * @brief Open a module image.
 *
 * @param path Path of the shared library to load.
 * @return The image handle, or ::FASTDL_IMAGE_INVALID on failure.
 */
static inline fastdl_image_t
fastdl_image_open(const char *path)
{
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

/**
 * @brief Look up an exported symbol of a module image.
 *
 * @param image Image handle.
 * @param name  Symbol name.
 * @return The symbol address, or NULL when it is missing.
 */
static inline void *
fastdl_image_symbol(fastdl_image_t image, const char *name)
{
#if defined(_WIN32)
    return (void *)GetProcAddress(image, name);
#else
    return dlsym(image, name);
#endif
}

/** @brief Release a module image. */
static inline void
fastdl_image_close(fastdl_image_t image)
{
#if defined(_WIN32)
    FreeLibrary(image);
#else
    dlclose(image);
#endif
}

/** @brief Text of the last image error. */
static inline const char *
fastdl_image_error(void)
{
#if defined(_WIN32)
    static char text[512];
    DWORD code = GetLastError();
    DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                  code, 0, text, (DWORD)sizeof text, NULL);
    if (!length)
    {
        text[0] = 0;
    }
    return text;
#else
    const char *text = dlerror();
    return text ? text : "unknown image error";
#endif
}

#if !defined(_WIN32)
/**
 * @brief Count the entries of a procfs directory.
 *
 * @param path Directory to read.
 * @return Number of entries, or -1 when the directory cannot be read.
 */
static inline int
fastdl_proc_entries(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    int count = 0;

    if (!dir)
    {
        return -1;
    }
    while ((entry = readdir(dir)))
    {
        if (entry->d_name[0] != '.')
        {
            ++count;
        }
    }
    closedir(dir);
    return count;
}
#endif

/**
 * @brief Count the open descriptors of this process.
 *
 * @return Descriptor count, or -1 when it cannot be read.
 */
static inline int
fastdl_handle_count(void)
{
#if defined(_WIN32)
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count))
    {
        return -1;
    }
    return (int)count;
#else
    return fastdl_proc_entries("/proc/self/fd");
#endif
}

/**
 * @brief Count the threads of this process.
 *
 * @return Thread count, or -1 when it cannot be read.
 */
static inline int
fastdl_thread_count(void)
{
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    DWORD owner = GetCurrentProcessId();
    int count = 0;

    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    entry.dwSize = sizeof entry;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            /* The snapshot holds every thread of the machine: keep ours. */
            if (entry.th32OwnerProcessID == owner)
            {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
#else
    return fastdl_proc_entries("/proc/self/task");
#endif
}

/** @brief Identity of the calling thread. */
static inline fastdl_thread_id_t
fastdl_thread_self(void)
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return pthread_self();
#endif
}

/**
 * @brief Tell whether two thread identities are the same thread.
 *
 * @param left  First identity.
 * @param right Second identity.
 * @return Nonzero when both name the same thread.
 */
static inline int
fastdl_thread_equal(fastdl_thread_id_t left, fastdl_thread_id_t right)
{
#if defined(_WIN32)
    return left == right;
#else
    return pthread_equal(left, right) != 0;
#endif
}

/**
 * @brief Resolve a path to its absolute form.
 *
 * @param path Path to resolve.
 * @param out  Buffer that receives the absolute path.
 * @param size Size of @p out in bytes.
 * @return 0 on success, -1 on failure.
 */
static inline int
fastdl_real_path(const char *path, char *out, size_t size)
{
#if defined(_WIN32)
    DWORD length = GetFullPathNameA(path, (DWORD)size, out, NULL);
    return length > 0 && length < size ? 0 : -1;
#else
    (void)size;
    return realpath(path, out) ? 0 : -1;
#endif
}

/**
 * @brief Copy the current working directory into a buffer.
 *
 * @param out  Buffer that receives the directory.
 * @param size Size of @p out in bytes.
 * @return 0 on success, -1 on failure.
 */
static inline int
fastdl_current_directory(char *out, size_t size)
{
#if defined(_WIN32)
    DWORD length = GetCurrentDirectoryA((DWORD)size, out);
    return length > 0 && length < size ? 0 : -1;
#else
    return getcwd(out, size) ? 0 : -1;
#endif
}

/** @brief A pair of descriptors kept open for the lifetime of the test. */
typedef struct fastdl_sentinel
{
#if defined(_WIN32)
    /** Read end of the pair. */
    HANDLE read_end;
    /** Write end of the pair. */
    HANDLE write_end;
#else
    /** Read end of the pair. */
    int read_end;
    /** Write end of the pair. */
    int write_end;
#endif
} fastdl_sentinel_t;

/**
 * @brief Open a pair of descriptors.
 *
 * @param sentinel Pair to fill.
 * @return 0 on success, -1 on failure.
 */
static inline int
fastdl_sentinel_open(fastdl_sentinel_t *sentinel)
{
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes;

    attributes.nLength = sizeof attributes;
    attributes.lpSecurityDescriptor = NULL;
    attributes.bInheritHandle = FALSE;
    if (!CreatePipe(&sentinel->read_end, &sentinel->write_end, &attributes, 0))
    {
        return -1;
    }
    return 0;
#else
    int pair[2];

    if (pipe(pair))
    {
        return -1;
    }
    sentinel->read_end = pair[0];
    sentinel->write_end = pair[1];
    return 0;
#endif
}

/**
 * @brief Tell whether both sentinel descriptors are still usable.
 *
 * @param sentinel Pair to inspect.
 * @return Nonzero when both ends are open.
 */
static inline int
fastdl_sentinel_valid(const fastdl_sentinel_t *sentinel)
{
#if defined(_WIN32)
    DWORD flags = 0;
    return GetHandleInformation(sentinel->read_end, &flags) &&
           GetHandleInformation(sentinel->write_end, &flags);
#else
    return fcntl(sentinel->read_end, F_GETFD) >= 0 && fcntl(sentinel->write_end, F_GETFD) >= 0;
#endif
}

/** @brief Close the sentinel pair. */
static inline void
fastdl_sentinel_close(fastdl_sentinel_t *sentinel)
{
#if defined(_WIN32)
    CloseHandle(sentinel->read_end);
    CloseHandle(sentinel->write_end);
#else
    close(sentinel->read_end);
    close(sentinel->write_end);
#endif
}

/** @brief Sleep for a number of milliseconds. */
static inline void
fastdl_sleep_ms(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000u);
#endif
}

/** @brief Name of the platform the harness runs on. */
static inline const char *
fastdl_platform_name(void)
{
#if FASTDL_TEST_LINUX
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "posix";
#endif
}

/**
 * @brief Linux-only checks this platform cannot make at all.
 *
 * @return Comma-separated names, or "none".
 */
static inline const char *
fastdl_skipped_checks(void)
{
#if FASTDL_TEST_LINUX
    return "none";
#else
    return "no_new_privs, sched_getaffinity, affinity_mask, rlimit_core, sigpipe_handler, "
           "fork_children";
#endif
}

/**
 * @brief Linux-only counters this platform replaces with its own.
 *
 * @return Comma-separated names, or "none".
 */
static inline const char *
fastdl_substituted_checks(void)
{
#if FASTDL_TEST_LINUX
    return "none";
#else
    return "proc_self_fd_count->GetProcessHandleCount, proc_self_task_count->thread snapshot";
#endif
}

#endif /* FASTDL_TEST_PLATFORM_H */
