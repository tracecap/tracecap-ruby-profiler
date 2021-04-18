#include <stdint.h>

#define MAX_FRAMES 2048

struct ruby_sample {
    struct {
        size_t total;
        size_t free;
    } object_space;
};
