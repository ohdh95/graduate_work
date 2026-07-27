#define _GNU_SOURCE

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <numaif.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <link.h>
#include <limits.h>

#define PAGEMAP_ENTRY 8             // Bytes per pagemap entry
#define PFN_MASK     ((1ULL << 55) - 1)
#define PAGE_SHIFT   12


#ifdef DEBUG
    #define debug(...) \
        do {fprintf(stderr, __VA_ARGS__);} while(0)    
#else
    #define debug(...)
#endif

static uint64_t virt_to_phys(uint64_t vaddr) {
#ifdef DEBUG    
    char pagemap_path[64];
    uint64_t file_offset, entry;
    int fd;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("open pagemap");
        return -1;
    }

    file_offset = (vaddr / sysconf(_SC_PAGESIZE)) * PAGEMAP_ENTRY;
    if (lseek(fd, file_offset, SEEK_SET) < 0) {
        perror("lseek pagemap");
        close(fd);
        return -1;
    }

    if (read(fd, &entry, PAGEMAP_ENTRY) != PAGEMAP_ENTRY) {
        perror("read pagemap");
        close(fd);
        return -1;
    }

    close(fd);

    uint64_t pfn = entry & PFN_MASK;
    return (pfn << PAGE_SHIFT) | (vaddr & ((1ULL << PAGE_SHIFT) - 1));
#else
    return 0;
#endif
}

static inline void __drop_page_cache(int fd) {
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static int __pin(const char *filename) {
    int old_mode = 0;
    unsigned long oldmask, nodemask;
    
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        debug("open %s failed\n", filename);
        perror("open");
        close(fd);
        return -1;
    }

    off_t length = lseek(fd, 0, SEEK_END);

    // __drop_page_cache(fd);
	if (get_mempolicy(&old_mode, &oldmask, sizeof(oldmask) * 8, NULL, 0) != 0) {
        perror("get_mempolicy");
        return -1;
    }
    
    nodemask = 1UL << 0; // node 0
    if (set_mempolicy(MPOL_BIND, &nodemask, sizeof(nodemask) * 8) != 0) {
        perror("set_mempolicy");
        return 1;
    }
    void *addr = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    
    if (mlock(addr, length) < 0) {
        perror("mlock");
        munmap(addr, length);
        close(fd);
        return -1;
    }

    if (set_mempolicy(old_mode, &oldmask, sizeof(oldmask) * 8) != 0) {
        perror("restore set_mempolicy");
        return 1;
    }

    debug("\tfile %s (%lu bytes) va: %p, pa: 0x%lx\n", filename, length, addr, virt_to_phys(addr));
    // close(fd);
    return 0;
}

char *resolve_exec_path(const char *file, char *const envp[]) {
    static char fullpath[PATH_MAX];

    // If absolute or contains '/', handle directly
    if (file[0] == '/' || strchr(file, '/')) {
        if (realpath(file, fullpath)) return fullpath;
        return NULL;
    }

    // Else: search in PATH
    const char *path_env = NULL;
    for (int i = 0; envp[i]; i++) {
        if (strncmp(envp[i], "PATH=", 5) == 0) {
            path_env = envp[i] + 5;
            break;
        }
    }
    if (!path_env) path_env = "/bin:/usr/bin";  // fallback

    char *paths = strdup(path_env);
    char *dir = strtok(paths, ":");
    while (dir) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, file);
        if (access(fullpath, X_OK) == 0) {
            free(paths);
            return fullpath;
        }
        dir = strtok(NULL, ":");
    }
    free(paths);
    return NULL;
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    static int (*real_execve)(const char *, char *const[], char *const[]) = NULL;
    if (!real_execve) {
        real_execve = dlsym(RTLD_NEXT, "execve");
    }
    
    char *resolved = resolve_exec_path(pathname, envp);
    if (!resolved) {
        debug("%s resolve exec path failed\n", pathname);
        return -1;
    }

    if (__pin(resolved)) {
		debug("pin %s failed\n", pathname);
		return -1;
    }
    
    return real_execve(resolved, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    return execve(file, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    return execve(file, argv, envp);
}

static char *resolve_full_path_from_dlpi_addr(void *addr) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return NULL;

    char *resolved = NULL;
    char line[4096];
    uintptr_t target = (uintptr_t)addr;

    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[5], path[PATH_MAX] = {0};

        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %s", &start, &end, perms, path) >= 3) {
            if (target >= start && target < end && strlen(path) > 0 && path[0] == '/') {
                resolved = strdup(path);
                break;
            }
        }
    }

    fclose(fp);
    return resolved;
}

static int pin_loaded_library(struct dl_phdr_info *info, size_t size, void *data) {
    const char *resolved = info->dlpi_name;

    if (!resolved || strlen(resolved) == 0 || resolved[0] != '/') {
        // Try to resolve based on address
        resolved = resolve_full_path_from_dlpi_addr((void *)info->dlpi_addr);
    }
    
    if (resolved) {
        if (__pin(resolved)) {
            debug("pin failed: %s\n", resolved);
        }

        if (resolved != info->dlpi_name)
            free((void *)resolved);
    }

    return 0;
}

static void pin_all_loaded_libraries() {
    dl_iterate_phdr(pin_loaded_library, NULL);
}

__attribute__((constructor))
void pin() {
    pin_all_loaded_libraries();
}