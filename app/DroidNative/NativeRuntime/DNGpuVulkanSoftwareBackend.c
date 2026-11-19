/*
 * Deterministic software GPU backend for host-side tests.
 *
 * This file is compiled into the iOS app only as an empty translation
 * unit: on Apple platforms the real backend is the Swift/Metal one.
 * Host tests define DN_GPU_BACKEND_SOFTWARE=1 (including on macOS) so
 * the Vulkan core can be exercised end to end without Apple SDKs.
 */
#if defined(DN_GPU_BACKEND_SOFTWARE) || !defined(__APPLE__)

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "DNGpuVulkanBackend.h"

#include <stdlib.h>
#include <string.h>

struct dn_sw_backend {
    int reserved;
};

struct dn_sw_buffer {
    uint8_t *data;
    uint64_t size;
};

struct dn_sw_image {
    uint8_t *data;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t bytes_per_pixel;
    uint32_t row_pitch;
    uint64_t size;
};

static uint32_t dn_sw_bpp(uint32_t format)
{
    switch (format) {
    case DN_GPU_FORMAT_RGBA8:
    case DN_GPU_FORMAT_BGRA8:
        return 4;
    case DN_GPU_FORMAT_R8:
        return 1;
    default:
        return 0;
    }
}

static int dn_sw_range_ok(uint64_t total, uint64_t offset, uint64_t size)
{
    return offset <= total && size <= total - offset;
}

static uint8_t dn_sw_float_to_byte(float value)
{
    if (!(value > 0.0f))
        return 0;
    if (value >= 1.0f)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

int dn_gpu_backend_probe(void)
{
    return 1;
}

void *dn_gpu_backend_create(void)
{
    return calloc(1, sizeof(struct dn_sw_backend));
}

void dn_gpu_backend_destroy(void *backend)
{
    free(backend);
}

void dn_gpu_backend_wait_idle(void *backend)
{
    (void)backend;
}

void *dn_gpu_backend_create_buffer(void *backend, uint64_t size, void **out_cpu_ptr)
{
    (void)backend;
    if (out_cpu_ptr)
        *out_cpu_ptr = NULL;
    if (size > (uint64_t)SIZE_MAX)
        return NULL;
    struct dn_sw_buffer *buffer = (struct dn_sw_buffer *)calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;
    buffer->size = size;
    buffer->data = (uint8_t *)calloc(1, size ? (size_t)size : 1);
    if (!buffer->data) {
        free(buffer);
        return NULL;
    }
    if (out_cpu_ptr)
        *out_cpu_ptr = buffer->data;
    return buffer;
}

void dn_gpu_backend_destroy_buffer(void *backend, void *buffer_ptr)
{
    (void)backend;
    struct dn_sw_buffer *buffer = (struct dn_sw_buffer *)buffer_ptr;
    if (!buffer)
        return;
    free(buffer->data);
    free(buffer);
}

void *dn_gpu_backend_create_image(void *backend, uint32_t width, uint32_t height,
                                  uint32_t backend_format)
{
    (void)backend;
    uint32_t bpp = dn_sw_bpp(backend_format);
    if (!bpp || width == 0 || height == 0)
        return NULL;
    uint64_t row_pitch = (uint64_t)width * bpp;
    uint64_t size = row_pitch * height;
    if (row_pitch > UINT32_MAX || size > (uint64_t)SIZE_MAX)
        return NULL;
    struct dn_sw_image *image = (struct dn_sw_image *)calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    image->data = (uint8_t *)calloc(1, (size_t)size);
    if (!image->data) {
        free(image);
        return NULL;
    }
    image->width = width;
    image->height = height;
    image->format = backend_format;
    image->bytes_per_pixel = bpp;
    image->row_pitch = (uint32_t)row_pitch;
    image->size = size;
    return image;
}

void dn_gpu_backend_destroy_image(void *backend, void *image_ptr)
{
    (void)backend;
    struct dn_sw_image *image = (struct dn_sw_image *)image_ptr;
    if (!image)
        return;
    free(image->data);
    free(image);
}

int dn_gpu_backend_fill_buffer(void *backend, void *buffer_ptr, uint64_t offset,
                               uint64_t size, uint32_t data)
{
    (void)backend;
    struct dn_sw_buffer *buffer = (struct dn_sw_buffer *)buffer_ptr;
    if (!buffer || !dn_sw_range_ok(buffer->size, offset, size))
        return -1;
    uint64_t i = 0;
    while (i + 4 <= size) {
        memcpy(buffer->data + offset + i, &data, 4);
        i += 4;
    }
    if (i < size)
        memcpy(buffer->data + offset + i, &data, (size_t)(size - i));
    return 0;
}

int dn_gpu_backend_copy_buffer(void *backend, void *src_ptr, uint64_t src_offset,
                               void *dst_ptr, uint64_t dst_offset, uint64_t size)
{
    (void)backend;
    struct dn_sw_buffer *src = (struct dn_sw_buffer *)src_ptr;
    struct dn_sw_buffer *dst = (struct dn_sw_buffer *)dst_ptr;
    if (!src || !dst)
        return -1;
    if (!dn_sw_range_ok(src->size, src_offset, size) ||
        !dn_sw_range_ok(dst->size, dst_offset, size))
        return -1;
    memmove(dst->data + dst_offset, src->data + src_offset, (size_t)size);
    return 0;
}

int dn_gpu_backend_clear_color_image(void *backend, void *image_ptr,
                                     uint32_t backend_format,
                                     float red, float green, float blue, float alpha)
{
    (void)backend;
    struct dn_sw_image *image = (struct dn_sw_image *)image_ptr;
    if (!image || image->format != backend_format)
        return -1;
    uint8_t r = dn_sw_float_to_byte(red);
    uint8_t g = dn_sw_float_to_byte(green);
    uint8_t b = dn_sw_float_to_byte(blue);
    uint8_t a = dn_sw_float_to_byte(alpha);
    for (uint32_t y = 0; y < image->height; ++y) {
        uint8_t *row = image->data + (uint64_t)y * image->row_pitch;
        for (uint32_t x = 0; x < image->width; ++x) {
            uint8_t *px = row + (uint64_t)x * image->bytes_per_pixel;
            if (backend_format == DN_GPU_FORMAT_RGBA8) {
                px[0] = r; px[1] = g; px[2] = b; px[3] = a;
            } else if (backend_format == DN_GPU_FORMAT_BGRA8) {
                px[0] = b; px[1] = g; px[2] = r; px[3] = a;
            } else {
                px[0] = r;
            }
        }
    }
    return 0;
}

static int dn_sw_image_rect_ok(const struct dn_sw_image *image, uint32_t x,
                               uint32_t y, uint32_t width, uint32_t height)
{
    return image && width > 0 && height > 0 &&
           x <= image->width && width <= image->width - x &&
           y <= image->height && height <= image->height - y;
}

int dn_gpu_backend_copy_buffer_to_image(void *backend, void *buffer_ptr,
                                        uint64_t buffer_offset,
                                        uint32_t buffer_row_pitch_bytes,
                                        void *image_ptr, uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height)
{
    (void)backend;
    struct dn_sw_buffer *buffer = (struct dn_sw_buffer *)buffer_ptr;
    struct dn_sw_image *image = (struct dn_sw_image *)image_ptr;
    if (!buffer || !dn_sw_image_rect_ok(image, x, y, width, height))
        return -1;
    uint32_t tight_row = width * image->bytes_per_pixel;
    uint32_t row_pitch = buffer_row_pitch_bytes ? buffer_row_pitch_bytes : tight_row;
    if (row_pitch < tight_row)
        return -1;
    uint64_t needed = (uint64_t)(height - 1) * row_pitch + tight_row;
    if (!dn_sw_range_ok(buffer->size, buffer_offset, needed))
        return -1;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = buffer->data + buffer_offset + (uint64_t)row * row_pitch;
        uint8_t *dst = image->data + (uint64_t)(y + row) * image->row_pitch +
                       (uint64_t)x * image->bytes_per_pixel;
        memcpy(dst, src, tight_row);
    }
    return 0;
}

int dn_gpu_backend_copy_image_to_buffer(void *backend, void *image_ptr,
                                        uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height,
                                        void *buffer_ptr, uint64_t buffer_offset,
                                        uint32_t buffer_row_pitch_bytes)
{
    (void)backend;
    struct dn_sw_image *image = (struct dn_sw_image *)image_ptr;
    struct dn_sw_buffer *buffer = (struct dn_sw_buffer *)buffer_ptr;
    if (!buffer || !dn_sw_image_rect_ok(image, x, y, width, height))
        return -1;
    uint32_t tight_row = width * image->bytes_per_pixel;
    uint32_t row_pitch = buffer_row_pitch_bytes ? buffer_row_pitch_bytes : tight_row;
    if (row_pitch < tight_row)
        return -1;
    uint64_t needed = (uint64_t)(height - 1) * row_pitch + tight_row;
    if (!dn_sw_range_ok(buffer->size, buffer_offset, needed))
        return -1;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = image->data + (uint64_t)(y + row) * image->row_pitch +
                             (uint64_t)x * image->bytes_per_pixel;
        uint8_t *dst = buffer->data + buffer_offset + (uint64_t)row * row_pitch;
        memcpy(dst, src, tight_row);
    }
    return 0;
}

int dn_gpu_backend_copy_image(void *backend, void *src_ptr,
                              uint32_t src_x, uint32_t src_y,
                              void *dst_ptr, uint32_t dst_x, uint32_t dst_y,
                              uint32_t width, uint32_t height)
{
    (void)backend;
    struct dn_sw_image *src = (struct dn_sw_image *)src_ptr;
    struct dn_sw_image *dst = (struct dn_sw_image *)dst_ptr;
    if (!dn_sw_image_rect_ok(src, src_x, src_y, width, height) ||
        !dn_sw_image_rect_ok(dst, dst_x, dst_y, width, height) ||
        src->bytes_per_pixel != dst->bytes_per_pixel)
        return -1;
    uint32_t row_bytes = width * src->bytes_per_pixel;
    if (src == dst) {
        /* Same-image copies are validated as non-semantic overlap cases by
         * the core for 0.3; still use a temporary row to be deterministic. */
        uint8_t *tmp = (uint8_t *)malloc(row_bytes);
        if (!tmp)
            return -1;
        for (uint32_t row = 0; row < height; ++row) {
            const uint8_t *s = src->data + (uint64_t)(src_y + row) * src->row_pitch +
                               (uint64_t)src_x * src->bytes_per_pixel;
            uint8_t *d = dst->data + (uint64_t)(dst_y + row) * dst->row_pitch +
                         (uint64_t)dst_x * dst->bytes_per_pixel;
            memcpy(tmp, s, row_bytes);
            memcpy(d, tmp, row_bytes);
        }
        free(tmp);
        return 0;
    }
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *s = src->data + (uint64_t)(src_y + row) * src->row_pitch +
                           (uint64_t)src_x * src->bytes_per_pixel;
        uint8_t *d = dst->data + (uint64_t)(dst_y + row) * dst->row_pitch +
                     (uint64_t)dst_x * dst->bytes_per_pixel;
        memcpy(d, s, row_bytes);
    }
    return 0;
}

#endif /* DN_GPU_BACKEND_SOFTWARE || !__APPLE__ */
