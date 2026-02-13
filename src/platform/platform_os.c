#include "platform_os.h"

#if defined(__unix__) || defined(__APPLE__)

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
#include <pthread.h>
#define LR_PLATFORM_CAN_USE_MAP_JIT 1
#else
#define LR_PLATFORM_CAN_USE_MAP_JIT 0
#endif

void *lr_platform_alloc_jit_code(size_t len, bool *out_map_jit_enabled) {
    void *map = MAP_FAILED;

    if (out_map_jit_enabled)
        *out_map_jit_enabled = false;

#if LR_PLATFORM_CAN_USE_MAP_JIT
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    prot |= PROT_EXEC;
    flags |= MAP_JIT;
    map = mmap(NULL, len, prot, flags, -1, 0);
    if (map != MAP_FAILED) {
        pthread_jit_write_protect_np(0);
        if (out_map_jit_enabled)
            *out_map_jit_enabled = true;
        return map;
    }
#endif

    map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return NULL;
    return map;
}

void *lr_platform_alloc_rw(size_t len) {
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return NULL;
    return map;
}

int lr_platform_free_pages(void *ptr, size_t len) {
    if (!ptr || len == 0)
        return -1;
    return munmap(ptr, len);
}

int lr_platform_jit_make_writable(void *code, size_t len, bool map_jit_enabled) {
    if (!code || len == 0)
        return -1;
    if (map_jit_enabled) {
#if LR_PLATFORM_CAN_USE_MAP_JIT
        pthread_jit_write_protect_np(0);
        return 0;
#else
        return -1;
#endif
    }
    return mprotect(code, len, PROT_READ | PROT_WRITE);
}

int lr_platform_jit_make_executable(void *code, size_t len, bool map_jit_enabled,
                                    const void *clear_begin, const void *clear_end) {
    if (!code || len == 0)
        return -1;
    if (clear_begin && clear_end && clear_begin < clear_end) {
        __builtin___clear_cache((char *)clear_begin, (char *)clear_end);
    }
    if (map_jit_enabled) {
#if LR_PLATFORM_CAN_USE_MAP_JIT
        pthread_jit_write_protect_np(1);
        return 0;
#else
        return -1;
#endif
    }
    return mprotect(code, len, PROT_READ | PROT_EXEC);
}

uint64_t lr_platform_time_ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0)
        mach_timebase_info(&info);
    uint64_t t = mach_absolute_time();
    return (uint64_t)((__uint128_t)t * info.numer / info.denom);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void *lr_platform_dlopen(const char *path) {
    if (!path || !path[0])
        return NULL;
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

int lr_platform_dlclose(void *handle) {
    if (!handle)
        return -1;
    return dlclose(handle);
}

void *lr_platform_dlsym(void *handle, const char *name) {
    if (!handle || !name || !name[0])
        return NULL;
    return dlsym(handle, name);
}

void *lr_platform_dlsym_default(const char *name) {
    if (!name || !name[0])
        return NULL;
    return dlsym(RTLD_DEFAULT, name);
}

int lr_platform_run_process(char *const argv[], bool quiet, int *out_status) {
    if (!argv || !argv[0])
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)dup2(devnull, STDOUT_FILENO);
                (void)dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    if (out_status) {
        if (WIFEXITED(status)) {
            *out_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *out_status = 128 + WTERMSIG(status);
        } else {
            *out_status = -1;
        }
    }
    return 0;
}

#else

void *lr_platform_alloc_jit_code(size_t len, bool *out_map_jit_enabled) {
    (void)len;
    if (out_map_jit_enabled)
        *out_map_jit_enabled = false;
    return NULL;
}

void *lr_platform_alloc_rw(size_t len) {
    (void)len;
    return NULL;
}

int lr_platform_free_pages(void *ptr, size_t len) {
    (void)ptr;
    (void)len;
    return -1;
}

int lr_platform_jit_make_writable(void *code, size_t len, bool map_jit_enabled) {
    (void)code;
    (void)len;
    (void)map_jit_enabled;
    return -1;
}

int lr_platform_jit_make_executable(void *code, size_t len, bool map_jit_enabled,
                                    const void *clear_begin, const void *clear_end) {
    (void)code;
    (void)len;
    (void)map_jit_enabled;
    (void)clear_begin;
    (void)clear_end;
    return -1;
}

uint64_t lr_platform_time_ns(void) {
    return 0;
}

void *lr_platform_dlopen(const char *path) {
    (void)path;
    return NULL;
}

int lr_platform_dlclose(void *handle) {
    (void)handle;
    return -1;
}

void *lr_platform_dlsym(void *handle, const char *name) {
    (void)handle;
    (void)name;
    return NULL;
}

void *lr_platform_dlsym_default(const char *name) {
    (void)name;
    return NULL;
}

int lr_platform_run_process(char *const argv[], bool quiet, int *out_status) {
    (void)argv;
    (void)quiet;
    if (out_status)
        *out_status = -1;
    return -1;
}

#endif
