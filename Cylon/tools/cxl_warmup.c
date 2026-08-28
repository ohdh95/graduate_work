#include <fcntl.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_MMAP_SIZE_GIB 96ULL

int main(int argc, char **argv)
{
    uint64_t size_gib = DEFAULT_MMAP_SIZE_GIB;

    if (argc == 2) {
        char *end = NULL;
        unsigned long long parsed = strtoull(argv[1], &end, 10);

        if (*argv[1] == '\0' || *end != '\0' || parsed == 0) {
            fprintf(stderr, "usage: %s [size_GiB]\n", argv[0]);
            return EXIT_FAILURE;
        }
        size_gib = parsed;
    } else if (argc > 2) {
        fprintf(stderr, "usage: %s [size_GiB]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const uint64_t size = size_gib * 1024ULL * 1024ULL * 1024ULL;

    printf("FEMU CXL-SSD warm-up: touching one byte per 4 KiB page\n");
    printf("Size: %llu GiB\n", (unsigned long long)size_gib);

    int fd = open("/dev/dax0.0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dax0.0");
        return EXIT_FAILURE;
    }

    uint8_t *cxl = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (cxl == MAP_FAILED) {
        perror("mmap /dev/dax0.0");
        close(fd);
        return EXIT_FAILURE;
    }

#pragma omp parallel for num_threads(8) schedule(static)
    for (uint64_t offset = 0; offset < size; offset += 4096) {
        cxl[offset] = 0;
    }

    if (munmap(cxl, size) != 0) {
        perror("munmap");
        close(fd);
        return EXIT_FAILURE;
    }

    close(fd);
    puts("Done");
    return EXIT_SUCCESS;
}
