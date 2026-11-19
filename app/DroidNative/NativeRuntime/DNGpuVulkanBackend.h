#ifndef DN_GPU_VULKAN_BACKEND_H
#define DN_GPU_VULKAN_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal GPU backend used by DNGpuVulkan.c.
 *
 * The Vulkan core is portable C.  On iOS this interface is implemented
 * by DNGpuVulkanMetalBackend.swift with MTLDevice/MTLCommandQueue; in
 * host tests it is implemented by DNGpuVulkanSoftwareBackend.c.  The
 * backend knows nothing about Vulkan handles: it owns opaque buffers
 * and images and executes already-validated transfer/clear work.
 */

enum {
    DN_GPU_FORMAT_RGBA8 = 1,
    DN_GPU_FORMAT_BGRA8 = 2,
    DN_GPU_FORMAT_R8 = 3
};

int dn_gpu_backend_probe(void);
void *dn_gpu_backend_create(void);
void dn_gpu_backend_destroy(void *backend);
void dn_gpu_backend_wait_idle(void *backend);

void *dn_gpu_backend_create_buffer(void *backend, uint64_t size, void **out_cpu_ptr);
void dn_gpu_backend_destroy_buffer(void *backend, void *buffer);

void *dn_gpu_backend_create_image(void *backend, uint32_t width, uint32_t height,
                                  uint32_t backend_format);
void dn_gpu_backend_destroy_image(void *backend, void *image);

int dn_gpu_backend_fill_buffer(void *backend, void *buffer, uint64_t offset,
                               uint64_t size, uint32_t data);
int dn_gpu_backend_copy_buffer(void *backend, void *src_buffer, uint64_t src_offset,
                               void *dst_buffer, uint64_t dst_offset, uint64_t size);
int dn_gpu_backend_clear_color_image(void *backend, void *image,
                                     uint32_t backend_format,
                                     float red, float green, float blue, float alpha);
int dn_gpu_backend_copy_buffer_to_image(void *backend, void *buffer,
                                        uint64_t buffer_offset,
                                        uint32_t buffer_row_pitch_bytes,
                                        void *image, uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height);
int dn_gpu_backend_copy_image_to_buffer(void *backend, void *image,
                                        uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height,
                                        void *buffer, uint64_t buffer_offset,
                                        uint32_t buffer_row_pitch_bytes);
int dn_gpu_backend_copy_image(void *backend, void *src_image,
                              uint32_t src_x, uint32_t src_y,
                              void *dst_image, uint32_t dst_x, uint32_t dst_y,
                              uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* DN_GPU_VULKAN_BACKEND_H */
