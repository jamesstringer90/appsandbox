#ifndef ASB_DISPLAY_GPU_H
#define ASB_DISPLAY_GPU_H

#include <stdint.h>
#include "protocol.h"

struct display_gpu;

struct display_gpu *display_gpu_create(int drm_fd);
void display_gpu_destroy(struct display_gpu *gpu);
int display_gpu_event(struct display_gpu *gpu);
int display_gpu_active(struct display_gpu *gpu);
int display_gpu_healthy(struct display_gpu *gpu);
int display_gpu_connect(struct display_gpu *gpu, int socket_fd,
                        const DisplayGpuPacket *query, uint32_t width, uint32_t height,
                        int resize, uint32_t size_mode);
void display_gpu_disconnect(struct display_gpu *gpu);
int display_gpu_frame(struct display_gpu *gpu);
int display_gpu_readback(struct display_gpu *gpu, const void **pixels,
                         uint32_t *width, uint32_t *height, uint32_t *stride);

#endif
