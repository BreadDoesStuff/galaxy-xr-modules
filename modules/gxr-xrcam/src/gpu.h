#pragma once
#include <stdint.h>
typedef struct {
    void *dpy, *ctx, *surf;
    unsigned prog, src, dst, fbo;
    int dw, dh;
} gpu_t;
int  gpu_init(gpu_t *g);
int  gpu_convert(gpu_t *g, void *ahb, int w, int h, uint8_t *rgba_out);
void gpu_destroy(gpu_t *g);
