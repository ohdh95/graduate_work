#include <sys/ioctl.h>                                                                                                                              [21/237]
#include <stdint.h>
#include <time.h>

#define MMAP_SIZE ((uint64_t)96*1024*1024*1024)

#include <omp.h>
int main(int argc, char** argv){

    uint64_t sz = MMAP_SIZE;
    if (argc==2)
        sz = (uint64_t)atoi(argv[1])*1024*1024*1024;
    }

    uint64_t *cxl = (uint64_t*)mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (cxl == MAP_FAILED) {
        perror("mmap: ");
        exit(0);
    }
    int a = 0;
    printf("Size: %.1f GB\n", sz/1024./1024./1024.);

    #pragma omp parallel for num_threads(8)
    for (uint64_t i=0;i<sz/8;i+=4096/8) {
        cxl[i] = 0;
    }

    munmap(cxl, sz);

    close(fd);
    printf("Done\n");
    return 0;
}