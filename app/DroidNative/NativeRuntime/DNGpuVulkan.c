/*
 * DroidNative Vulkan-on-Metal driver core (iDroid 0.3).
 *
 * Portable C implementation of the small Vulkan 1.0 subset declared in
 * DNGpuVulkan.h.  Android ARM64 code bound by DN's ELF mini-linker
 * calls these vk* entry points directly; the actual GPU work is
 * delegated to a backend (Metal on iOS, deterministic software in host
 * tests).  Queue submission is synchronous in 0.3: commands are
 * validated while recording, executed in order on vkQueueSubmit, and
 * fences/semaphores change state only after the backend reports
 * completion.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Same Darwin feature-exposure rule as the rest of NativeRuntime:
 * strict POSIX feature selection must not hide Darwin declarations. */
#define _DARWIN_C_SOURCE 1
#endif

#include "DNGpuVulkan.h"
#include "DNGpuVulkanBackend.h"
#include "DNNativeShims.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DN_VK_MAGIC 0x444E564BU /* "DNVK" */
#define DN_VK_MAX_IMAGE_DIMENSION 4096U
#define DN_VK_MAX_ALLOCATION_SIZE (512ULL * 1024ULL * 1024ULL)
#define DN_VK_HEAP_SIZE (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define DN_VK_BUFFER_ALIGNMENT 16ULL
#define DN_VK_IMAGE_ALIGNMENT 256ULL
#define DN_VK_ALL_BUFFER_USAGE 0x000001FFU
#define DN_VK_ALL_IMAGE_USAGE 0x000000FFU
#define DN_VK_SUPPORTED_IMAGE_USAGE \
    (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | \
     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
#define DN_VK_ALL_PIPELINE_STAGES 0x0001FFFFU
#define DN_VK_ALL_ACCESS_FLAGS 0x0001FFFFU

_Static_assert(VK_FORMAT_R8_UNORM == 9, "Vulkan ABI format value changed");
_Static_assert(VK_FORMAT_R8G8B8A8_UNORM == 37, "Vulkan ABI format value changed");
_Static_assert(VK_FORMAT_B8G8R8A8_UNORM == 44, "Vulkan ABI format value changed");
_Static_assert(sizeof(VkPhysicalDeviceFeatures) == 55U * sizeof(VkBool32),
               "Vulkan 1.0 feature block must contain 55 booleans");
_Static_assert(sizeof(VkBufferCopy) == 24U, "VkBufferCopy ABI size");
_Static_assert(sizeof(VkBufferImageCopy) == 56U, "VkBufferImageCopy ABI size");
_Static_assert(sizeof(VkMemoryRequirements) == 24U, "VkMemoryRequirements ABI size");

enum dn_vk_object_type {
    DN_VK_OBJ_INSTANCE = 1,
    DN_VK_OBJ_PHYSICAL_DEVICE,
    DN_VK_OBJ_DEVICE,
    DN_VK_OBJ_QUEUE,
    DN_VK_OBJ_MEMORY,
    DN_VK_OBJ_BUFFER,
    DN_VK_OBJ_IMAGE,
    DN_VK_OBJ_IMAGE_VIEW,
    DN_VK_OBJ_COMMAND_POOL,
    DN_VK_OBJ_COMMAND_BUFFER,
    DN_VK_OBJ_FENCE,
    DN_VK_OBJ_SEMAPHORE
};

struct dn_vk_head {
    void *dispatch;
    uint32_t magic;
    uint32_t object_type;
    _Atomic uint32_t refcount;
};

static const void *dn_vk_dispatch_table_anchor;
#define DN_VK_DISPATCH ((void *)(uintptr_t)&dn_vk_dispatch_table_anchor)
#define DN_VK_CAST(type, handle) ((type *)(uintptr_t)(handle))
#define DN_VK_HANDLE(type, ptr) ((type)(uintptr_t)(ptr))

struct dn_vk_instance {
    struct dn_vk_head head;
    struct dn_vk_physical_device *physical;
    int backend_available;
    _Atomic VkBool32 destroyed;
};

struct dn_vk_physical_device {
    struct dn_vk_head head;
    struct dn_vk_instance *instance;
};

struct dn_vk_device {
    struct dn_vk_head head;
    struct dn_vk_instance *instance;
    struct dn_vk_physical_device *physical;
    struct dn_vk_queue *queue;
    void *backend;
    pthread_mutex_t lock;
    _Atomic VkBool32 destroyed;
    _Atomic VkBool32 lost;
};

struct dn_vk_queue {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    uint32_t family_index;
    uint32_t queue_index;
};

struct dn_vk_memory {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    VkDeviceSize size;
    uint32_t memory_type_index;
    void *backend_buffer;
    void *cpu_ptr;
    VkBool32 mapped;
    VkDeviceSize mapped_offset;
    VkDeviceSize mapped_size;
    _Atomic uint32_t buffer_bind_count;
    _Atomic VkBool32 image_bound;
};

struct dn_vk_buffer {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    struct dn_vk_memory *memory;
    VkDeviceSize memory_offset;
};

struct dn_vk_image {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    VkFormat format;
    uint32_t backend_format;
    uint32_t bytes_per_pixel;
    VkExtent3D extent;
    VkImageUsageFlags usage;
    VkImageLayout layout;
    VkDeviceSize requirement_size;
    void *backend_image;
    struct dn_vk_memory *memory;
    VkDeviceSize memory_offset;
};

struct dn_vk_image_view {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    struct dn_vk_image *image;
    VkFormat format;
    VkImageSubresourceRange range;
};

struct dn_vk_command_pool {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    VkCommandPoolCreateFlags flags;
    uint32_t queue_family_index;
    pthread_mutex_t lock;
    struct dn_vk_command_buffer **buffers;
    size_t buffer_count;
    size_t buffer_capacity;
};

enum dn_vk_command_kind {
    DN_VK_CMD_COPY_BUFFER = 1,
    DN_VK_CMD_FILL_BUFFER,
    DN_VK_CMD_UPDATE_BUFFER,
    DN_VK_CMD_CLEAR_COLOR_IMAGE,
    DN_VK_CMD_COPY_BUFFER_TO_IMAGE,
    DN_VK_CMD_COPY_IMAGE_TO_BUFFER,
    DN_VK_CMD_COPY_IMAGE,
    DN_VK_CMD_IMAGE_BARRIER
};

struct dn_vk_command {
    enum dn_vk_command_kind kind;
    struct dn_vk_buffer *src_buffer;
    struct dn_vk_buffer *dst_buffer;
    struct dn_vk_image *src_image;
    struct dn_vk_image *dst_image;
    struct dn_vk_image *image;
    VkDeviceSize src_offset;
    VkDeviceSize dst_offset;
    VkDeviceSize size;
    uint32_t data;
    void *owned_data;
    VkClearColorValue clear_color;
    VkImageLayout layout;
    VkImageLayout old_layout;
    VkImageLayout new_layout;
    uint32_t buffer_row_pitch_bytes;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t width;
    uint32_t height;
};

enum dn_vk_command_buffer_state {
    DN_VK_CB_INITIAL = 0,
    DN_VK_CB_RECORDING,
    DN_VK_CB_EXECUTABLE,
    DN_VK_CB_INVALID
};

struct dn_vk_command_buffer {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    struct dn_vk_command_pool *pool;
    enum dn_vk_command_buffer_state state;
    VkCommandBufferUsageFlags usage_flags;
    VkResult record_error;
    struct dn_vk_command *commands;
    size_t command_count;
    size_t command_capacity;
};

struct dn_vk_fence {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    _Atomic VkBool32 signaled;
};

struct dn_vk_semaphore {
    struct dn_vk_head head;
    struct dn_vk_device *device;
    _Atomic VkBool32 signaled;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void dn_vk_head_init(struct dn_vk_head *head, enum dn_vk_object_type type)
{
    memset(head, 0, sizeof(*head));
    head->dispatch = DN_VK_DISPATCH;
    head->magic = DN_VK_MAGIC;
    head->object_type = (uint32_t)type;
    atomic_init(&head->refcount, 1U);
}

static bool dn_vk_head_valid(const void *ptr, enum dn_vk_object_type type)
{
    if (!ptr)
        return false;
    const struct dn_vk_head *head = (const struct dn_vk_head *)ptr;
    return head->magic == DN_VK_MAGIC && head->object_type == (uint32_t)type;
}

static void dn_vk_retain(struct dn_vk_head *head)
{
    if (head)
        atomic_fetch_add_explicit(&head->refcount, 1U, memory_order_relaxed);
}

static bool dn_vk_release(struct dn_vk_head *head)
{
    if (!head)
        return false;
    return atomic_fetch_sub_explicit(&head->refcount, 1U, memory_order_acq_rel) == 1U;
}

static bool dn_vk_range_ok(VkDeviceSize total, VkDeviceSize offset, VkDeviceSize size)
{
    return offset <= total && size <= total - offset;
}

static bool dn_vk_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

static bool dn_vk_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a)
        return false;
    *out = a + b;
    return true;
}

static uint32_t dn_vk_format_bpp(VkFormat format, uint32_t *backend_format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        if (backend_format) *backend_format = DN_GPU_FORMAT_R8;
        return 1;
    case VK_FORMAT_R8G8B8A8_UNORM:
        if (backend_format) *backend_format = DN_GPU_FORMAT_RGBA8;
        return 4;
    case VK_FORMAT_B8G8R8A8_UNORM:
        if (backend_format) *backend_format = DN_GPU_FORMAT_BGRA8;
        return 4;
    default:
        if (backend_format) *backend_format = 0;
        return 0;
    }
}

static bool dn_vk_format_supported(VkFormat format)
{
    return dn_vk_format_bpp(format, NULL) != 0;
}

static bool dn_vk_layout_supported(VkImageLayout layout)
{
    return layout >= VK_IMAGE_LAYOUT_UNDEFINED &&
           layout <= VK_IMAGE_LAYOUT_PREINITIALIZED;
}

static bool dn_vk_queue_family_list_ok(uint32_t count, const uint32_t *indices)
{
    if (count == 0)
        return true;
    if (!indices)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (indices[i] != 0)
            return false;
    }
    return true;
}

static void dn_vk_instance_release(struct dn_vk_instance *instance);
static void dn_vk_device_release(struct dn_vk_device *device);
static void dn_vk_memory_release(struct dn_vk_memory *memory);
static void dn_vk_buffer_release(struct dn_vk_buffer *buffer);
static void dn_vk_image_release(struct dn_vk_image *image);

static void dn_vk_retain_device(struct dn_vk_device *device)
{
    if (device)
        dn_vk_retain(&device->head);
}

static void dn_vk_retain_instance(struct dn_vk_instance *instance)
{
    if (instance)
        dn_vk_retain(&instance->head);
}

static void dn_vk_instance_release(struct dn_vk_instance *instance)
{
    if (!instance || !dn_vk_release(&instance->head))
        return;
    free(instance->physical);
    free(instance);
}

static void dn_vk_device_release(struct dn_vk_device *device)
{
    if (!device || !dn_vk_release(&device->head))
        return;
    struct dn_vk_instance *instance = device->instance;
    if (device->backend) {
        dn_gpu_backend_wait_idle(device->backend);
        dn_gpu_backend_destroy(device->backend);
    }
    free(device->queue);
    pthread_mutex_destroy(&device->lock);
    free(device);
    dn_vk_instance_release(instance);
}

static void dn_vk_memory_release(struct dn_vk_memory *memory)
{
    if (!memory || !dn_vk_release(&memory->head))
        return;
    struct dn_vk_device *device = memory->device;
    if (device && memory->backend_buffer)
        dn_gpu_backend_destroy_buffer(device->backend, memory->backend_buffer);
    free(memory);
    dn_vk_device_release(device);
}

static void dn_vk_buffer_release(struct dn_vk_buffer *buffer)
{
    if (!buffer || !dn_vk_release(&buffer->head))
        return;
    struct dn_vk_memory *memory = buffer->memory;
    struct dn_vk_device *device = buffer->device;
    if (memory)
        atomic_fetch_sub_explicit(&memory->buffer_bind_count, 1U, memory_order_acq_rel);
    free(buffer);
    dn_vk_memory_release(memory);
    dn_vk_device_release(device);
}

static void dn_vk_image_release(struct dn_vk_image *image)
{
    if (!image || !dn_vk_release(&image->head))
        return;
    struct dn_vk_memory *memory = image->memory;
    struct dn_vk_device *device = image->device;
    if (memory)
        atomic_store_explicit(&memory->image_bound, VK_FALSE, memory_order_release);
    if (device && image->backend_image)
        dn_gpu_backend_destroy_image(device->backend, image->backend_image);
    free(image);
    dn_vk_memory_release(memory);
    dn_vk_device_release(device);
}

static struct dn_vk_instance *dn_vk_get_instance(VkInstance handle)
{
    struct dn_vk_instance *instance = DN_VK_CAST(struct dn_vk_instance, handle);
    return dn_vk_head_valid(instance, DN_VK_OBJ_INSTANCE) ? instance : NULL;
}

static struct dn_vk_physical_device *dn_vk_get_physical(VkPhysicalDevice handle)
{
    struct dn_vk_physical_device *physical =
        DN_VK_CAST(struct dn_vk_physical_device, handle);
    return dn_vk_head_valid(physical, DN_VK_OBJ_PHYSICAL_DEVICE) ? physical : NULL;
}

static struct dn_vk_device *dn_vk_get_device(VkDevice handle)
{
    struct dn_vk_device *device = DN_VK_CAST(struct dn_vk_device, handle);
    return dn_vk_head_valid(device, DN_VK_OBJ_DEVICE) ? device : NULL;
}

static struct dn_vk_queue *dn_vk_get_queue(VkQueue handle)
{
    struct dn_vk_queue *queue = DN_VK_CAST(struct dn_vk_queue, handle);
    return dn_vk_head_valid(queue, DN_VK_OBJ_QUEUE) ? queue : NULL;
}

static bool dn_vk_device_usable(struct dn_vk_device *device)
{
    return device && device->backend &&
           atomic_load_explicit(&device->destroyed, memory_order_acquire) == VK_FALSE &&
           atomic_load_explicit(&device->lost, memory_order_acquire) == VK_FALSE;
}

static VkResult dn_vk_device_result(struct dn_vk_device *device)
{
    if (!device)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (atomic_load_explicit(&device->lost, memory_order_acquire) != VK_FALSE ||
        atomic_load_explicit(&device->destroyed, memory_order_acquire) != VK_FALSE)
        return VK_ERROR_DEVICE_LOST;
    return VK_SUCCESS;
}

static struct dn_vk_memory *dn_vk_get_memory(VkDeviceMemory handle,
                                             struct dn_vk_device *device)
{
    struct dn_vk_memory *memory = DN_VK_CAST(struct dn_vk_memory, handle);
    if (!dn_vk_head_valid(memory, DN_VK_OBJ_MEMORY))
        return NULL;
    if (device && memory->device != device)
        return NULL;
    return memory;
}

static struct dn_vk_buffer *dn_vk_get_buffer(VkBuffer handle,
                                             struct dn_vk_device *device)
{
    struct dn_vk_buffer *buffer = DN_VK_CAST(struct dn_vk_buffer, handle);
    if (!dn_vk_head_valid(buffer, DN_VK_OBJ_BUFFER))
        return NULL;
    if (device && buffer->device != device)
        return NULL;
    return buffer;
}

static struct dn_vk_image *dn_vk_get_image(VkImage handle,
                                           struct dn_vk_device *device)
{
    struct dn_vk_image *image = DN_VK_CAST(struct dn_vk_image, handle);
    if (!dn_vk_head_valid(image, DN_VK_OBJ_IMAGE))
        return NULL;
    if (device && image->device != device)
        return NULL;
    return image;
}

static struct dn_vk_image_view *dn_vk_get_image_view(VkImageView handle,
                                                     struct dn_vk_device *device)
{
    struct dn_vk_image_view *view = DN_VK_CAST(struct dn_vk_image_view, handle);
    if (!dn_vk_head_valid(view, DN_VK_OBJ_IMAGE_VIEW))
        return NULL;
    if (device && view->device != device)
        return NULL;
    return view;
}

static struct dn_vk_command_pool *dn_vk_get_pool(VkCommandPool handle,
                                                 struct dn_vk_device *device)
{
    struct dn_vk_command_pool *pool = DN_VK_CAST(struct dn_vk_command_pool, handle);
    if (!dn_vk_head_valid(pool, DN_VK_OBJ_COMMAND_POOL))
        return NULL;
    if (device && pool->device != device)
        return NULL;
    return pool;
}

static struct dn_vk_command_buffer *dn_vk_get_command_buffer(VkCommandBuffer handle,
                                                             struct dn_vk_device *device)
{
    struct dn_vk_command_buffer *cb = DN_VK_CAST(struct dn_vk_command_buffer, handle);
    if (!dn_vk_head_valid(cb, DN_VK_OBJ_COMMAND_BUFFER))
        return NULL;
    if (device && cb->device != device)
        return NULL;
    return cb;
}

static struct dn_vk_fence *dn_vk_get_fence(VkFence handle,
                                           struct dn_vk_device *device)
{
    struct dn_vk_fence *fence = DN_VK_CAST(struct dn_vk_fence, handle);
    if (!dn_vk_head_valid(fence, DN_VK_OBJ_FENCE))
        return NULL;
    if (device && fence->device != device)
        return NULL;
    return fence;
}

static struct dn_vk_semaphore *dn_vk_get_semaphore(VkSemaphore handle,
                                                   struct dn_vk_device *device)
{
    struct dn_vk_semaphore *semaphore = DN_VK_CAST(struct dn_vk_semaphore, handle);
    if (!dn_vk_head_valid(semaphore, DN_VK_OBJ_SEMAPHORE))
        return NULL;
    if (device && semaphore->device != device)
        return NULL;
    return semaphore;
}

static bool dn_vk_features_any_enabled(const VkPhysicalDeviceFeatures *features)
{
    if (!features)
        return false;
    const VkBool32 *words = (const VkBool32 *)features;
    for (size_t i = 0; i < sizeof(*features) / sizeof(VkBool32); ++i) {
        if (words[i] != VK_FALSE)
            return true;
    }
    return false;
}

static uint64_t dn_vk_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void dn_vk_sleep_ns(uint64_t ns)
{
    struct timespec request;
    request.tv_sec = (time_t)(ns / 1000000000ULL);
    request.tv_nsec = (long)(ns % 1000000000ULL);
    (void)nanosleep(&request, NULL);
}

/* ------------------------------------------------------------------ */
/* Instance / physical device                                          */
/* ------------------------------------------------------------------ */

VkResult vkEnumerateInstanceVersion(uint32_t *pApiVersion)
{
    if (!pApiVersion)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                                uint32_t *pPropertyCount,
                                                VkExtensionProperties *pProperties)
{
    (void)pProperties;
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName && pLayerName[0] != '\0')
        return VK_ERROR_LAYER_NOT_PRESENT;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                            VkLayerProperties *pProperties)
{
    (void)pProperties;
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkInstance *pInstance)
{
    (void)pAllocator;
    if (!pCreateInfo || !pInstance)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pInstance = VK_NULL_HANDLE;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO || pCreateInfo->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->enabledLayerCount != 0)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (pCreateInfo->enabledExtensionCount != 0)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (pCreateInfo->pApplicationInfo) {
        const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;
        if (app->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || app->pNext)
            return VK_ERROR_INITIALIZATION_FAILED;
        uint32_t api = app->apiVersion;
        if (api != 0) {
            uint32_t variant = api >> 29U;
            uint32_t major = VK_VERSION_MAJOR(api);
            uint32_t minor = VK_VERSION_MINOR(api);
            if (variant != 0 || major > 1 || (major == 1 && minor > 0))
                return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
    }

    struct dn_vk_instance *instance = (struct dn_vk_instance *)calloc(1, sizeof(*instance));
    if (!instance)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct dn_vk_physical_device *physical =
        (struct dn_vk_physical_device *)calloc(1, sizeof(*physical));
    if (!physical) {
        free(instance);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    dn_vk_head_init(&instance->head, DN_VK_OBJ_INSTANCE);
    atomic_init(&instance->destroyed, VK_FALSE);
    dn_vk_head_init(&physical->head, DN_VK_OBJ_PHYSICAL_DEVICE);
    physical->instance = instance;
    instance->physical = physical;
    instance->backend_available = dn_gpu_backend_probe() ? 1 : 0;
    *pInstance = DN_VK_HANDLE(VkInstance, instance);
    return VK_SUCCESS;
}

void vkDestroyInstance(VkInstance handle, const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_instance *instance = dn_vk_get_instance(handle);
    if (!instance)
        return;
    atomic_store_explicit(&instance->destroyed, VK_TRUE, memory_order_release);
    dn_vk_instance_release(instance);
}

VkResult vkEnumeratePhysicalDevices(VkInstance handle,
                                    uint32_t *pPhysicalDeviceCount,
                                    VkPhysicalDevice *pPhysicalDevices)
{
    struct dn_vk_instance *instance = dn_vk_get_instance(handle);
    if (!instance || !pPhysicalDeviceCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t total = instance->backend_available ? 1U : 0U;
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = total;
        return VK_SUCCESS;
    }
    uint32_t capacity = *pPhysicalDeviceCount;
    uint32_t written = capacity < total ? capacity : total;
    if (written > 0)
        pPhysicalDevices[0] = DN_VK_HANDLE(VkPhysicalDevice, instance->physical);
    *pPhysicalDeviceCount = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static void dn_vk_fill_limits(VkPhysicalDeviceLimits *limits)
{
    memset(limits, 0, sizeof(*limits));
    limits->maxImageDimension1D = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxImageDimension2D = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxImageDimension3D = 1;
    limits->maxImageDimensionCube = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxImageArrayLayers = 1;
    limits->maxTexelBufferElements = 1U << 24U;
    limits->maxUniformBufferRange = 65536U;
    limits->maxStorageBufferRange = 134217728U;
    limits->maxPushConstantsSize = 0;
    limits->maxMemoryAllocationCount = 4096U;
    limits->maxSamplerAllocationCount = 0;
    limits->bufferImageGranularity = 1;
    limits->sparseAddressSpaceSize = 0;
    limits->maxFragmentOutputAttachments = 1;
    limits->maxComputeWorkGroupCount[0] = 65535U;
    limits->maxComputeWorkGroupCount[1] = 65535U;
    limits->maxComputeWorkGroupCount[2] = 65535U;
    limits->maxComputeWorkGroupInvocations = 1024U;
    limits->maxComputeWorkGroupSize[0] = 1024U;
    limits->maxComputeWorkGroupSize[1] = 1024U;
    limits->maxComputeWorkGroupSize[2] = 64U;
    limits->maxViewports = 1;
    limits->maxViewportDimensions[0] = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxViewportDimensions[1] = DN_VK_MAX_IMAGE_DIMENSION;
    limits->viewportBoundsRange[0] = -8192.0f;
    limits->viewportBoundsRange[1] = 8191.0f;
    limits->minMemoryMapAlignment = 256U;
    limits->minTexelBufferOffsetAlignment = 16U;
    limits->minUniformBufferOffsetAlignment = 16U;
    limits->minStorageBufferOffsetAlignment = 16U;
    limits->maxFramebufferWidth = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxFramebufferHeight = DN_VK_MAX_IMAGE_DIMENSION;
    limits->maxFramebufferLayers = 1;
    limits->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->maxColorAttachments = 1;
    limits->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits->maxSampleMaskWords = 1;
    limits->timestampComputeAndGraphics = VK_FALSE;
    limits->timestampPeriod = 1.0f;
    limits->discreteQueuePriorities = 1;
    limits->pointSizeRange[0] = 1.0f;
    limits->pointSizeRange[1] = 1.0f;
    limits->lineWidthRange[0] = 1.0f;
    limits->lineWidthRange[1] = 1.0f;
    limits->standardSampleLocations = VK_TRUE;
    limits->optimalBufferCopyOffsetAlignment = 1;
    limits->optimalBufferCopyRowPitchAlignment = 1;
    limits->nonCoherentAtomSize = 1;
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice handle,
                                   VkPhysicalDeviceProperties *pProperties)
{
    if (!pProperties)
        return;
    memset(pProperties, 0, sizeof(*pProperties));
    if (!dn_vk_get_physical(handle))
        return;
    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = VK_MAKE_VERSION(0, 3, 0);
    pProperties->vendorID = 0x106BU; /* Apple */
    pProperties->deviceID = 0x0000A18BU;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    (void)snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
                   "Apple GPU (DroidNative Vulkan-on-Metal 0.3)");
    memcpy(pProperties->pipelineCacheUUID, "DNVK-0.3-IPHONE!", VK_UUID_SIZE);
    dn_vk_fill_limits(&pProperties->limits);
}

void vkGetPhysicalDeviceFeatures(VkPhysicalDevice handle,
                                 VkPhysicalDeviceFeatures *pFeatures)
{
    if (!pFeatures)
        return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    (void)handle;
}

void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice handle,
                                              uint32_t *pQueueFamilyPropertyCount,
                                              VkQueueFamilyProperties *pQueueFamilyProperties)
{
    if (!pQueueFamilyPropertyCount)
        return;
    if (!dn_vk_get_physical(handle)) {
        *pQueueFamilyPropertyCount = 0;
        return;
    }
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount == 0)
        return;
    memset(&pQueueFamilyProperties[0], 0, sizeof(pQueueFamilyProperties[0]));
    pQueueFamilyProperties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT |
                                           VK_QUEUE_COMPUTE_BIT |
                                           VK_QUEUE_TRANSFER_BIT;
    pQueueFamilyProperties[0].queueCount = 1;
    pQueueFamilyProperties[0].timestampValidBits = 0;
    pQueueFamilyProperties[0].minImageTransferGranularity.width = 1;
    pQueueFamilyProperties[0].minImageTransferGranularity.height = 1;
    pQueueFamilyProperties[0].minImageTransferGranularity.depth = 1;
    *pQueueFamilyPropertyCount = 1;
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice handle,
                                         VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
    if (!pMemoryProperties)
        return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    if (!dn_vk_get_physical(handle))
        return;
    pMemoryProperties->memoryTypeCount = 1;
    pMemoryProperties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = DN_VK_HEAP_SIZE;
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice handle,
                                         VkFormat format,
                                         VkFormatProperties *pFormatProperties)
{
    if (!pFormatProperties)
        return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    if (!dn_vk_get_physical(handle) || !dn_vk_format_supported(format))
        return;
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                    VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    pFormatProperties->linearTilingFeatures = features;
    pFormatProperties->optimalTilingFeatures = features;
    pFormatProperties->bufferFeatures = 0;
}

VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice handle,
                                                  VkFormat format,
                                                  VkImageType type,
                                                  VkImageTiling tiling,
                                                  VkImageUsageFlags usage,
                                                  VkImageCreateFlags flags,
                                                  VkImageFormatProperties *pImageFormatProperties)
{
    if (!dn_vk_get_physical(handle) || !pImageFormatProperties)
        return VK_ERROR_INITIALIZATION_FAILED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    if (!dn_vk_format_supported(format) || type != VK_IMAGE_TYPE_2D || flags != 0 ||
        usage == 0 || (usage & ~DN_VK_SUPPORTED_IMAGE_USAGE) != 0 ||
        (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    pImageFormatProperties->maxExtent.width = DN_VK_MAX_IMAGE_DIMENSION;
    pImageFormatProperties->maxExtent.height = DN_VK_MAX_IMAGE_DIMENSION;
    pImageFormatProperties->maxExtent.depth = 1;
    pImageFormatProperties->maxMipLevels = 1;
    pImageFormatProperties->maxArrayLayers = 1;
    pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize = DN_VK_MAX_ALLOCATION_SIZE;
    return VK_SUCCESS;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice handle,
                                              const char *pLayerName,
                                              uint32_t *pPropertyCount,
                                              VkExtensionProperties *pProperties)
{
    (void)pProperties;
    if (!dn_vk_get_physical(handle) || !pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName && pLayerName[0] != '\0')
        return VK_ERROR_LAYER_NOT_PRESENT;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice handle,
                                          uint32_t *pPropertyCount,
                                          VkLayerProperties *pProperties)
{
    (void)pProperties;
    if (!dn_vk_get_physical(handle) || !pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Device / queue                                                      */
/* ------------------------------------------------------------------ */

VkResult vkCreateDevice(VkPhysicalDevice physical_handle,
                        const VkDeviceCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkDevice *pDevice)
{
    (void)pAllocator;
    struct dn_vk_physical_device *physical = dn_vk_get_physical(physical_handle);
    if (!physical || !pCreateInfo || !pDevice)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pDevice = VK_NULL_HANDLE;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || pCreateInfo->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->enabledLayerCount != 0)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (pCreateInfo->enabledExtensionCount != 0)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (dn_vk_features_any_enabled(pCreateInfo->pEnabledFeatures))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pCreateInfo->queueCreateInfoCount != 1 || !pCreateInfo->pQueueCreateInfos)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkDeviceQueueCreateInfo *queue_info = &pCreateInfo->pQueueCreateInfos[0];
    if (queue_info->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || queue_info->pNext ||
        queue_info->flags != 0 || queue_info->queueFamilyIndex != 0 ||
        queue_info->queueCount != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (queue_info->pQueuePriorities) {
        float priority = queue_info->pQueuePriorities[0];
        if (!(priority >= 0.0f && priority <= 1.0f))
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    struct dn_vk_device *device = (struct dn_vk_device *)calloc(1, sizeof(*device));
    if (!device)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct dn_vk_queue *queue = (struct dn_vk_queue *)calloc(1, sizeof(*queue));
    if (!queue) {
        free(device);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    void *backend = dn_gpu_backend_create();
    if (!backend) {
        free(queue);
        free(device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    dn_vk_head_init(&device->head, DN_VK_OBJ_DEVICE);
    dn_vk_head_init(&queue->head, DN_VK_OBJ_QUEUE);
    atomic_init(&device->destroyed, VK_FALSE);
    atomic_init(&device->lost, VK_FALSE);
    pthread_mutex_init(&device->lock, NULL);
    device->instance = physical->instance;
    device->physical = physical;
    device->backend = backend;
    device->queue = queue;
    queue->device = device;
    queue->family_index = 0;
    queue->queue_index = 0;
    dn_vk_retain_instance(device->instance);
    *pDevice = DN_VK_HANDLE(VkDevice, device);
    return VK_SUCCESS;
}

void vkDestroyDevice(VkDevice handle, const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device)
        return;
    if (device->backend)
        dn_gpu_backend_wait_idle(device->backend);
    atomic_store_explicit(&device->destroyed, VK_TRUE, memory_order_release);
    dn_vk_device_release(device);
}

void vkGetDeviceQueue(VkDevice handle, uint32_t queueFamilyIndex,
                      uint32_t queueIndex, VkQueue *pQueue)
{
    if (!pQueue)
        return;
    *pQueue = VK_NULL_HANDLE;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || queueFamilyIndex != 0 || queueIndex != 0)
        return;
    *pQueue = DN_VK_HANDLE(VkQueue, device->queue);
}

VkResult vkDeviceWaitIdle(VkDevice handle)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    dn_gpu_backend_wait_idle(device->backend);
    return VK_SUCCESS;
}

VkResult vkQueueWaitIdle(VkQueue handle)
{
    struct dn_vk_queue *queue = dn_vk_get_queue(handle);
    if (!queue)
        return VK_ERROR_INITIALIZATION_FAILED;
    return vkDeviceWaitIdle(DN_VK_HANDLE(VkDevice, queue->device));
}

/* ------------------------------------------------------------------ */
/* Memory / buffers / images                                           */
/* ------------------------------------------------------------------ */

VkResult vkAllocateMemory(VkDevice handle, const VkMemoryAllocateInfo *pAllocateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkDeviceMemory *pMemory)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pAllocateInfo || !pMemory)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pMemory = VK_NULL_HANDLE;
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    if (pAllocateInfo->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || pAllocateInfo->pNext ||
        pAllocateInfo->memoryTypeIndex != 0 || pAllocateInfo->allocationSize == 0 ||
        pAllocateInfo->allocationSize > DN_VK_MAX_ALLOCATION_SIZE)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    struct dn_vk_memory *memory = (struct dn_vk_memory *)calloc(1, sizeof(*memory));
    if (!memory)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    void *cpu_ptr = NULL;
    void *backend_buffer = dn_gpu_backend_create_buffer(device->backend,
                                                        pAllocateInfo->allocationSize,
                                                        &cpu_ptr);
    if (!backend_buffer) {
        free(memory);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    dn_vk_head_init(&memory->head, DN_VK_OBJ_MEMORY);
    atomic_init(&memory->buffer_bind_count, 0U);
    atomic_init(&memory->image_bound, VK_FALSE);
    memory->device = device;
    memory->size = pAllocateInfo->allocationSize;
    memory->memory_type_index = pAllocateInfo->memoryTypeIndex;
    memory->backend_buffer = backend_buffer;
    memory->cpu_ptr = cpu_ptr;
    dn_vk_retain_device(device);
    *pMemory = DN_VK_HANDLE(VkDeviceMemory, memory);
    return VK_SUCCESS;
}

void vkFreeMemory(VkDevice handle, VkDeviceMemory memory_handle,
                  const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_memory *memory = dn_vk_get_memory(memory_handle, device);
    if (memory)
        dn_vk_memory_release(memory);
}

VkResult vkMapMemory(VkDevice handle, VkDeviceMemory memory_handle, VkDeviceSize offset,
                     VkDeviceSize size, VkFlags flags, void **ppData)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_memory *memory = dn_vk_get_memory(memory_handle, device);
    if (!memory || !ppData || flags != 0)
        return VK_ERROR_MEMORY_MAP_FAILED;
    *ppData = NULL;
    if (!dn_vk_device_usable(device) || memory->mapped || !memory->cpu_ptr)
        return VK_ERROR_MEMORY_MAP_FAILED;
    if (atomic_load_explicit(&memory->image_bound, memory_order_acquire) != VK_FALSE)
        return VK_ERROR_MEMORY_MAP_FAILED;
    VkDeviceSize effective_size = size == VK_WHOLE_SIZE ? memory->size - offset : size;
    if (offset > memory->size || effective_size == 0 ||
        !dn_vk_range_ok(memory->size, offset, effective_size))
        return VK_ERROR_MEMORY_MAP_FAILED;
    memory->mapped = VK_TRUE;
    memory->mapped_offset = offset;
    memory->mapped_size = effective_size;
    *ppData = (uint8_t *)memory->cpu_ptr + offset;
    return VK_SUCCESS;
}

void vkUnmapMemory(VkDevice handle, VkDeviceMemory memory_handle)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_memory *memory = dn_vk_get_memory(memory_handle, device);
    if (!memory)
        return;
    memory->mapped = VK_FALSE;
    memory->mapped_offset = 0;
    memory->mapped_size = 0;
}

static VkResult dn_vk_check_mapped_ranges(struct dn_vk_device *device,
                                          uint32_t count, const VkMappedMemoryRange *ranges)
{
    if (count > 0 && !ranges)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; ++i) {
        const VkMappedMemoryRange *range = &ranges[i];
        if (range->sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || range->pNext)
            return VK_ERROR_INITIALIZATION_FAILED;
        struct dn_vk_memory *memory = dn_vk_get_memory(range->memory, device);
        if (!memory)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkDeviceSize effective = range->size == VK_WHOLE_SIZE ?
            (range->offset <= memory->size ? memory->size - range->offset : 0) : range->size;
        if (!dn_vk_range_ok(memory->size, range->offset, effective))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult vkFlushMappedMemoryRanges(VkDevice handle, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device)
        return VK_ERROR_INITIALIZATION_FAILED;
    return dn_vk_check_mapped_ranges(device, memoryRangeCount, pMemoryRanges);
}

VkResult vkInvalidateMappedMemoryRanges(VkDevice handle, uint32_t memoryRangeCount,
                                        const VkMappedMemoryRange *pMemoryRanges)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device)
        return VK_ERROR_INITIALIZATION_FAILED;
    return dn_vk_check_mapped_ranges(device, memoryRangeCount, pMemoryRanges);
}

VkResult vkCreateBuffer(VkDevice handle, const VkBufferCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pBuffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pBuffer = VK_NULL_HANDLE;
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || pCreateInfo->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->flags != 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (pCreateInfo->size == 0 || pCreateInfo->size > DN_VK_MAX_ALLOCATION_SIZE ||
        pCreateInfo->usage == 0 || (pCreateInfo->usage & ~DN_VK_ALL_BUFFER_USAGE) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
        if (!dn_vk_queue_family_list_ok(pCreateInfo->queueFamilyIndexCount,
                                        pCreateInfo->pQueueFamilyIndices))
            return VK_ERROR_INITIALIZATION_FAILED;
    } else if (pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    struct dn_vk_buffer *buffer = (struct dn_vk_buffer *)calloc(1, sizeof(*buffer));
    if (!buffer)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dn_vk_head_init(&buffer->head, DN_VK_OBJ_BUFFER);
    buffer->device = device;
    buffer->size = pCreateInfo->size;
    buffer->usage = pCreateInfo->usage;
    dn_vk_retain_device(device);
    *pBuffer = DN_VK_HANDLE(VkBuffer, buffer);
    return VK_SUCCESS;
}

void vkDestroyBuffer(VkDevice handle, VkBuffer buffer_handle,
                     const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_buffer *buffer = dn_vk_get_buffer(buffer_handle, device);
    if (buffer)
        dn_vk_buffer_release(buffer);
}

void vkGetBufferMemoryRequirements(VkDevice handle, VkBuffer buffer_handle,
                                   VkMemoryRequirements *pMemoryRequirements)
{
    if (!pMemoryRequirements)
        return;
    memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_buffer *buffer = dn_vk_get_buffer(buffer_handle, device);
    if (!buffer)
        return;
    pMemoryRequirements->size = buffer->size;
    pMemoryRequirements->alignment = DN_VK_BUFFER_ALIGNMENT;
    pMemoryRequirements->memoryTypeBits = 1U;
}

VkResult vkBindBufferMemory(VkDevice handle, VkBuffer buffer_handle,
                            VkDeviceMemory memory_handle, VkDeviceSize memoryOffset)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_buffer *buffer = dn_vk_get_buffer(buffer_handle, device);
    struct dn_vk_memory *memory = dn_vk_get_memory(memory_handle, device);
    if (!buffer || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (buffer->memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (atomic_load_explicit(&memory->image_bound, memory_order_acquire) != VK_FALSE)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (memoryOffset % DN_VK_BUFFER_ALIGNMENT != 0 ||
        !dn_vk_range_ok(memory->size, memoryOffset, buffer->size))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    dn_vk_retain(&memory->head);
    atomic_fetch_add_explicit(&memory->buffer_bind_count, 1U, memory_order_acq_rel);
    buffer->memory = memory;
    buffer->memory_offset = memoryOffset;
    return VK_SUCCESS;
}

VkResult vkCreateImage(VkDevice handle, const VkImageCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pImage)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pImage = VK_NULL_HANDLE;
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || pCreateInfo->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->flags != 0)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t backend_format = 0;
    uint32_t bpp = dn_vk_format_bpp(pCreateInfo->format, &backend_format);
    if (!bpp)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
        pCreateInfo->extent.width == 0 || pCreateInfo->extent.height == 0 ||
        pCreateInfo->extent.width > DN_VK_MAX_IMAGE_DIMENSION ||
        pCreateInfo->extent.height > DN_VK_MAX_IMAGE_DIMENSION ||
        pCreateInfo->extent.depth != 1 || pCreateInfo->mipLevels != 1 ||
        pCreateInfo->arrayLayers != 1 || pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT ||
        (pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL &&
         pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR) ||
        pCreateInfo->usage == 0 || (pCreateInfo->usage & ~DN_VK_SUPPORTED_IMAGE_USAGE) != 0 ||
        (pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
         pCreateInfo->initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
        if (!dn_vk_queue_family_list_ok(pCreateInfo->queueFamilyIndexCount,
                                        pCreateInfo->pQueueFamilyIndices))
            return VK_ERROR_INITIALIZATION_FAILED;
    } else if (pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t row_bytes = 0;
    uint64_t requirement = 0;
    if (!dn_vk_mul_u64(pCreateInfo->extent.width, bpp, &row_bytes) ||
        !dn_vk_mul_u64(row_bytes, pCreateInfo->extent.height, &requirement) ||
        requirement == 0 || requirement > DN_VK_MAX_ALLOCATION_SIZE)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    struct dn_vk_image *image = (struct dn_vk_image *)calloc(1, sizeof(*image));
    if (!image)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    void *backend_image = dn_gpu_backend_create_image(device->backend,
                                                      pCreateInfo->extent.width,
                                                      pCreateInfo->extent.height,
                                                      backend_format);
    if (!backend_image) {
        free(image);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    dn_vk_head_init(&image->head, DN_VK_OBJ_IMAGE);
    image->device = device;
    image->format = pCreateInfo->format;
    image->backend_format = backend_format;
    image->bytes_per_pixel = bpp;
    image->extent = pCreateInfo->extent;
    image->usage = pCreateInfo->usage;
    image->layout = pCreateInfo->initialLayout;
    image->requirement_size = requirement;
    image->backend_image = backend_image;
    dn_vk_retain_device(device);
    *pImage = DN_VK_HANDLE(VkImage, image);
    return VK_SUCCESS;
}

void vkDestroyImage(VkDevice handle, VkImage image_handle,
                    const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_image *image = dn_vk_get_image(image_handle, device);
    if (image)
        dn_vk_image_release(image);
}

void vkGetImageMemoryRequirements(VkDevice handle, VkImage image_handle,
                                  VkMemoryRequirements *pMemoryRequirements)
{
    if (!pMemoryRequirements)
        return;
    memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_image *image = dn_vk_get_image(image_handle, device);
    if (!image)
        return;
    pMemoryRequirements->size = image->requirement_size;
    pMemoryRequirements->alignment = DN_VK_IMAGE_ALIGNMENT;
    pMemoryRequirements->memoryTypeBits = 1U;
}

VkResult vkBindImageMemory(VkDevice handle, VkImage image_handle,
                           VkDeviceMemory memory_handle, VkDeviceSize memoryOffset)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_image *image = dn_vk_get_image(image_handle, device);
    struct dn_vk_memory *memory = dn_vk_get_memory(memory_handle, device);
    if (!image || !memory || image->memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (atomic_load_explicit(&memory->image_bound, memory_order_acquire) != VK_FALSE ||
        atomic_load_explicit(&memory->buffer_bind_count, memory_order_acquire) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (memoryOffset % DN_VK_IMAGE_ALIGNMENT != 0 ||
        !dn_vk_range_ok(memory->size, memoryOffset, image->requirement_size))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    dn_vk_retain(&memory->head);
    atomic_store_explicit(&memory->image_bound, VK_TRUE, memory_order_release);
    image->memory = memory;
    image->memory_offset = memoryOffset;
    return VK_SUCCESS;
}

VkResult vkCreateImageView(VkDevice handle, const VkImageViewCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pView)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pView = VK_NULL_HANDLE;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags != 0 || pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_image *image = dn_vk_get_image(pCreateInfo->image, device);
    if (!image || pCreateInfo->format != image->format)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
    uint32_t level_count = range->levelCount == VK_REMAINING_MIP_LEVELS ? 1U : range->levelCount;
    uint32_t layer_count = range->layerCount == VK_REMAINING_ARRAY_LAYERS ? 1U : range->layerCount;
    if (range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel != 0 ||
        level_count != 1 || range->baseArrayLayer != 0 || layer_count != 1)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const VkComponentSwizzle swizzles[4] = {
        pCreateInfo->components.r, pCreateInfo->components.g,
        pCreateInfo->components.b, pCreateInfo->components.a
    };
    for (int i = 0; i < 4; ++i) {
        if (swizzles[i] < VK_COMPONENT_SWIZZLE_IDENTITY || swizzles[i] > VK_COMPONENT_SWIZZLE_A)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct dn_vk_image_view *view = (struct dn_vk_image_view *)calloc(1, sizeof(*view));
    if (!view)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dn_vk_head_init(&view->head, DN_VK_OBJ_IMAGE_VIEW);
    view->device = device;
    view->image = image;
    view->format = pCreateInfo->format;
    view->range = *range;
    dn_vk_retain(&image->head);
    dn_vk_retain_device(device);
    *pView = DN_VK_HANDLE(VkImageView, view);
    return VK_SUCCESS;
}

void vkDestroyImageView(VkDevice handle, VkImageView view_handle,
                        const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_image_view *view = dn_vk_get_image_view(view_handle, device);
    if (!view || !dn_vk_release(&view->head))
        return;
    struct dn_vk_image *image = view->image;
    struct dn_vk_device *view_device = view->device;
    free(view);
    dn_vk_image_release(image);
    dn_vk_device_release(view_device);
}

/* ------------------------------------------------------------------ */
/* Command pools / buffers / sync objects                              */
/* ------------------------------------------------------------------ */

static void dn_vk_command_release_refs(struct dn_vk_command *command)
{
    if (command->src_buffer)
        dn_vk_buffer_release(command->src_buffer);
    if (command->dst_buffer && command->dst_buffer != command->src_buffer)
        dn_vk_buffer_release(command->dst_buffer);
    if (command->src_image)
        dn_vk_image_release(command->src_image);
    if (command->dst_image && command->dst_image != command->src_image)
        dn_vk_image_release(command->dst_image);
    if (command->image && command->image != command->src_image &&
        command->image != command->dst_image)
        dn_vk_image_release(command->image);
    free(command->owned_data);
    memset(command, 0, sizeof(*command));
}

static void dn_vk_command_buffer_clear(struct dn_vk_command_buffer *cb, bool free_storage)
{
    for (size_t i = 0; i < cb->command_count; ++i)
        dn_vk_command_release_refs(&cb->commands[i]);
    cb->command_count = 0;
    cb->record_error = VK_SUCCESS;
    if (free_storage) {
        free(cb->commands);
        cb->commands = NULL;
        cb->command_capacity = 0;
    }
}

static void dn_vk_command_buffer_destroy(struct dn_vk_command_buffer *cb)
{
    if (!cb || !dn_vk_release(&cb->head))
        return;
    struct dn_vk_device *device = cb->device;
    dn_vk_command_buffer_clear(cb, true);
    free(cb);
    dn_vk_device_release(device);
}

static bool dn_vk_pool_add(struct dn_vk_command_pool *pool,
                           struct dn_vk_command_buffer *cb)
{
    pthread_mutex_lock(&pool->lock);
    if (pool->buffer_count == pool->buffer_capacity) {
        size_t next = pool->buffer_capacity ? pool->buffer_capacity * 2 : 4;
        struct dn_vk_command_buffer **grown =
            (struct dn_vk_command_buffer **)realloc(pool->buffers, next * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&pool->lock);
            return false;
        }
        pool->buffers = grown;
        pool->buffer_capacity = next;
    }
    pool->buffers[pool->buffer_count++] = cb;
    pthread_mutex_unlock(&pool->lock);
    return true;
}

static bool dn_vk_pool_remove(struct dn_vk_command_pool *pool,
                              struct dn_vk_command_buffer *cb)
{
    bool found = false;
    pthread_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->buffer_count; ++i) {
        if (pool->buffers[i] == cb) {
            pool->buffers[i] = pool->buffers[pool->buffer_count - 1];
            pool->buffer_count--;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
    return found;
}

VkResult vkCreateCommandPool(VkDevice handle, const VkCommandPoolCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkCommandPool *pCommandPool)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pCommandPool)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pCommandPool = VK_NULL_HANDLE;
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandPoolCreateFlags supported = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                               VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->queueFamilyIndex != 0 || (pCreateInfo->flags & ~supported) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_command_pool *pool = (struct dn_vk_command_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dn_vk_head_init(&pool->head, DN_VK_OBJ_COMMAND_POOL);
    pool->device = device;
    pool->flags = pCreateInfo->flags;
    pool->queue_family_index = 0;
    pthread_mutex_init(&pool->lock, NULL);
    dn_vk_retain_device(device);
    *pCommandPool = DN_VK_HANDLE(VkCommandPool, pool);
    return VK_SUCCESS;
}

void vkDestroyCommandPool(VkDevice handle, VkCommandPool pool_handle,
                          const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_command_pool *pool = dn_vk_get_pool(pool_handle, device);
    if (!pool || !dn_vk_release(&pool->head))
        return;
    struct dn_vk_device *pool_device = pool->device;
    for (size_t i = 0; i < pool->buffer_count; ++i)
        dn_vk_command_buffer_destroy(pool->buffers[i]);
    free(pool->buffers);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    dn_vk_device_release(pool_device);
}

VkResult vkResetCommandPool(VkDevice handle, VkCommandPool pool_handle,
                            VkCommandPoolResetFlags flags)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_command_pool *pool = dn_vk_get_pool(pool_handle, device);
    if (!pool || (flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&pool->lock);
    for (size_t i = 0; i < pool->buffer_count; ++i) {
        struct dn_vk_command_buffer *cb = pool->buffers[i];
        dn_vk_command_buffer_clear(cb, flags == VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
        cb->state = DN_VK_CB_INITIAL;
        cb->usage_flags = 0;
    }
    pthread_mutex_unlock(&pool->lock);
    return VK_SUCCESS;
}

VkResult vkAllocateCommandBuffers(VkDevice handle,
                                  const VkCommandBufferAllocateInfo *pAllocateInfo,
                                  VkCommandBuffer *pCommandBuffers)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pAllocateInfo || !pCommandBuffers ||
        pAllocateInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO ||
        pAllocateInfo->pNext || pAllocateInfo->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        pAllocateInfo->commandBufferCount == 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_command_pool *pool = dn_vk_get_pool(pAllocateInfo->commandPool, device);
    if (!pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
        pCommandBuffers[i] = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
        struct dn_vk_command_buffer *cb =
            (struct dn_vk_command_buffer *)calloc(1, sizeof(*cb));
        if (!cb) {
            for (uint32_t j = 0; j < i; ++j) {
                struct dn_vk_command_buffer *old =
                    DN_VK_CAST(struct dn_vk_command_buffer, pCommandBuffers[j]);
                if (dn_vk_pool_remove(pool, old))
                    dn_vk_command_buffer_destroy(old);
                pCommandBuffers[j] = VK_NULL_HANDLE;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        dn_vk_head_init(&cb->head, DN_VK_OBJ_COMMAND_BUFFER);
        cb->device = device;
        cb->pool = pool;
        cb->state = DN_VK_CB_INITIAL;
        cb->record_error = VK_SUCCESS;
        dn_vk_retain_device(device);
        if (!dn_vk_pool_add(pool, cb)) {
            dn_vk_command_buffer_destroy(cb);
            for (uint32_t j = 0; j < i; ++j) {
                struct dn_vk_command_buffer *old =
                    DN_VK_CAST(struct dn_vk_command_buffer, pCommandBuffers[j]);
                if (dn_vk_pool_remove(pool, old))
                    dn_vk_command_buffer_destroy(old);
                pCommandBuffers[j] = VK_NULL_HANDLE;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        pCommandBuffers[i] = DN_VK_HANDLE(VkCommandBuffer, cb);
    }
    return VK_SUCCESS;
}

void vkFreeCommandBuffers(VkDevice handle, VkCommandPool pool_handle,
                          uint32_t commandBufferCount,
                          const VkCommandBuffer *pCommandBuffers)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_command_pool *pool = dn_vk_get_pool(pool_handle, device);
    if (!pool || (commandBufferCount > 0 && !pCommandBuffers))
        return;
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(pCommandBuffers[i], device);
        if (cb && cb->pool == pool && dn_vk_pool_remove(pool, cb))
            dn_vk_command_buffer_destroy(cb);
    }
}

VkResult vkBeginCommandBuffer(VkCommandBuffer handle,
                              const VkCommandBufferBeginInfo *pBeginInfo)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!cb || !pBeginInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (cb->state != DN_VK_CB_INITIAL)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkCommandBufferUsageFlags supported = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                                VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    if (pBeginInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || pBeginInfo->pNext ||
        (pBeginInfo->flags & ~supported) != 0 || pBeginInfo->pInheritanceInfo != NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    dn_vk_command_buffer_clear(cb, false);
    cb->usage_flags = pBeginInfo->flags;
    cb->state = DN_VK_CB_RECORDING;
    return VK_SUCCESS;
}

VkResult vkEndCommandBuffer(VkCommandBuffer handle)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!cb || cb->state != DN_VK_CB_RECORDING)
        return VK_ERROR_INITIALIZATION_FAILED;
    cb->state = cb->record_error == VK_SUCCESS ? DN_VK_CB_EXECUTABLE : DN_VK_CB_INVALID;
    return cb->record_error;
}

VkResult vkResetCommandBuffer(VkCommandBuffer handle,
                              VkCommandBufferResetFlags flags)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!cb || !cb->pool || (flags & ~VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((cb->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (cb->state == DN_VK_CB_RECORDING)
        return VK_ERROR_INITIALIZATION_FAILED;
    dn_vk_command_buffer_clear(cb, flags == VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    cb->state = DN_VK_CB_INITIAL;
    cb->usage_flags = 0;
    return VK_SUCCESS;
}

VkResult vkCreateFence(VkDevice handle, const VkFenceCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pFence)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pFence = VK_NULL_HANDLE;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO || pCreateInfo->pNext ||
        (pCreateInfo->flags & ~VK_FENCE_CREATE_SIGNALED_BIT) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_fence *fence = (struct dn_vk_fence *)calloc(1, sizeof(*fence));
    if (!fence)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dn_vk_head_init(&fence->head, DN_VK_OBJ_FENCE);
    atomic_init(&fence->signaled,
                (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) ? VK_TRUE : VK_FALSE);
    fence->device = device;
    dn_vk_retain_device(device);
    *pFence = DN_VK_HANDLE(VkFence, fence);
    return VK_SUCCESS;
}

void vkDestroyFence(VkDevice handle, VkFence fence_handle,
                    const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_fence *fence = dn_vk_get_fence(fence_handle, device);
    if (!fence || !dn_vk_release(&fence->head))
        return;
    struct dn_vk_device *fence_device = fence->device;
    free(fence);
    dn_vk_device_release(fence_device);
}

VkResult vkResetFences(VkDevice handle, uint32_t fenceCount, const VkFence *pFences)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || (fenceCount > 0 && !pFences))
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; ++i) {
        if (!dn_vk_get_fence(pFences[i], device))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < fenceCount; ++i) {
        struct dn_vk_fence *fence = dn_vk_get_fence(pFences[i], device);
        atomic_store_explicit(&fence->signaled, VK_FALSE, memory_order_release);
    }
    return VK_SUCCESS;
}

VkResult vkGetFenceStatus(VkDevice handle, VkFence fence_handle)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_fence *fence = dn_vk_get_fence(fence_handle, device);
    if (!fence)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (atomic_load_explicit(&device->lost, memory_order_acquire) != VK_FALSE)
        return VK_ERROR_DEVICE_LOST;
    return atomic_load_explicit(&fence->signaled, memory_order_acquire) != VK_FALSE ?
        VK_SUCCESS : VK_NOT_READY;
}

VkResult vkWaitForFences(VkDevice handle, uint32_t fenceCount,
                         const VkFence *pFences, VkBool32 waitAll, uint64_t timeout)
{
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || fenceCount == 0 || !pFences)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < fenceCount; ++i) {
        if (!dn_vk_get_fence(pFences[i], device))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t deadline = UINT64_MAX;
    if (timeout != UINT64_MAX) {
        uint64_t now = dn_vk_monotonic_ns();
        deadline = timeout > UINT64_MAX - now ? UINT64_MAX : now + timeout;
    }
    for (;;) {
        if (atomic_load_explicit(&device->lost, memory_order_acquire) != VK_FALSE)
            return VK_ERROR_DEVICE_LOST;
        uint32_t signaled = 0;
        for (uint32_t i = 0; i < fenceCount; ++i) {
            struct dn_vk_fence *fence = dn_vk_get_fence(pFences[i], device);
            if (atomic_load_explicit(&fence->signaled, memory_order_acquire) != VK_FALSE)
                signaled++;
        }
        if (waitAll ? signaled == fenceCount : signaled > 0)
            return VK_SUCCESS;
        if (timeout == 0)
            return VK_NOT_READY;
        uint64_t now = dn_vk_monotonic_ns();
        if (now >= deadline)
            return VK_TIMEOUT;
        uint64_t remaining = deadline - now;
        dn_vk_sleep_ns(remaining < 1000000ULL ? remaining : 1000000ULL);
    }
}

VkResult vkCreateSemaphore(VkDevice handle, const VkSemaphoreCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    if (!device || !pCreateInfo || !pSemaphore)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pSemaphore = VK_NULL_HANDLE;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO || pCreateInfo->pNext ||
        pCreateInfo->flags != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_semaphore *semaphore = (struct dn_vk_semaphore *)calloc(1, sizeof(*semaphore));
    if (!semaphore)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dn_vk_head_init(&semaphore->head, DN_VK_OBJ_SEMAPHORE);
    atomic_init(&semaphore->signaled, VK_FALSE);
    semaphore->device = device;
    dn_vk_retain_device(device);
    *pSemaphore = DN_VK_HANDLE(VkSemaphore, semaphore);
    return VK_SUCCESS;
}

void vkDestroySemaphore(VkDevice handle, VkSemaphore semaphore_handle,
                        const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    struct dn_vk_device *device = dn_vk_get_device(handle);
    struct dn_vk_semaphore *semaphore = dn_vk_get_semaphore(semaphore_handle, device);
    if (!semaphore || !dn_vk_release(&semaphore->head))
        return;
    struct dn_vk_device *semaphore_device = semaphore->device;
    free(semaphore);
    dn_vk_device_release(semaphore_device);
}

/* ------------------------------------------------------------------ */
/* Command recording                                                   */
/* ------------------------------------------------------------------ */

static bool dn_vk_cmd_recording(struct dn_vk_command_buffer *cb)
{
    return cb && cb->state == DN_VK_CB_RECORDING;
}

static void dn_vk_cmd_fail(struct dn_vk_command_buffer *cb, VkResult result)
{
    if (cb && cb->record_error == VK_SUCCESS)
        cb->record_error = result;
}

static struct dn_vk_command *dn_vk_cmd_append(struct dn_vk_command_buffer *cb,
                                              enum dn_vk_command_kind kind)
{
    if (cb->command_count == cb->command_capacity) {
        size_t next = cb->command_capacity ? cb->command_capacity * 2 : 8;
        struct dn_vk_command *grown =
            (struct dn_vk_command *)realloc(cb->commands, next * sizeof(*grown));
        if (!grown) {
            dn_vk_cmd_fail(cb, VK_ERROR_OUT_OF_HOST_MEMORY);
            return NULL;
        }
        cb->commands = grown;
        cb->command_capacity = next;
    }
    struct dn_vk_command *command = &cb->commands[cb->command_count++];
    memset(command, 0, sizeof(*command));
    command->kind = kind;
    return command;
}

static bool dn_vk_buffer_bound_range(struct dn_vk_buffer *buffer, VkDeviceSize offset,
                                     VkDeviceSize size, VkBufferUsageFlags usage)
{
    return buffer && buffer->memory && buffer->memory->backend_buffer &&
           (buffer->usage & usage) == usage &&
           dn_vk_range_ok(buffer->size, offset, size) &&
           dn_vk_range_ok(buffer->memory->size, buffer->memory_offset + offset, size);
}

static bool dn_vk_image_subresource_ok(VkImageAspectFlags aspect, uint32_t mip,
                                       uint32_t base_layer, uint32_t layer_count,
                                       bool allow_remaining_layers)
{
    uint32_t layers = layer_count;
    if (allow_remaining_layers && layers == VK_REMAINING_ARRAY_LAYERS)
        layers = 1;
    return aspect == VK_IMAGE_ASPECT_COLOR_BIT && mip == 0 && base_layer == 0 && layers == 1;
}

static bool dn_vk_image_range_ok(const VkImageSubresourceRange *range)
{
    uint32_t levels = range->levelCount == VK_REMAINING_MIP_LEVELS ? 1U : range->levelCount;
    uint32_t layers = range->layerCount == VK_REMAINING_ARRAY_LAYERS ? 1U : range->layerCount;
    return range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && range->baseMipLevel == 0 &&
           levels == 1 && range->baseArrayLayer == 0 && layers == 1;
}

static bool dn_vk_image_rect_ok(struct dn_vk_image *image, VkOffset3D offset,
                                VkExtent3D extent)
{
    if (!image || !image->memory || !image->backend_image)
        return false;
    if (offset.x < 0 || offset.y < 0 || offset.z != 0 || extent.width == 0 ||
        extent.height == 0 || extent.depth != 1)
        return false;
    uint32_t x = (uint32_t)offset.x;
    uint32_t y = (uint32_t)offset.y;
    return x <= image->extent.width && extent.width <= image->extent.width - x &&
           y <= image->extent.height && extent.height <= image->extent.height - y;
}

static bool dn_vk_buffer_image_bytes_ok(struct dn_vk_buffer *buffer,
                                        struct dn_vk_image *image,
                                        const VkBufferImageCopy *region,
                                        VkBufferUsageFlags buffer_usage,
                                        uint32_t *out_row_pitch)
{
    if (!buffer || !image || !dn_vk_image_rect_ok(image, region->imageOffset, region->imageExtent))
        return false;
    if (!dn_vk_image_subresource_ok(region->imageSubresource.aspectMask,
                                    region->imageSubresource.mipLevel,
                                    region->imageSubresource.baseArrayLayer,
                                    region->imageSubresource.layerCount, false))
        return false;
    uint64_t tight_row = 0;
    if (!dn_vk_mul_u64(region->imageExtent.width, image->bytes_per_pixel, &tight_row) ||
        tight_row > UINT32_MAX)
        return false;
    uint64_t row_pitch = tight_row;
    if (region->bufferRowLength != 0) {
        if (region->bufferRowLength < region->imageExtent.width ||
            !dn_vk_mul_u64(region->bufferRowLength, image->bytes_per_pixel, &row_pitch) ||
            row_pitch > UINT32_MAX)
            return false;
    }
    uint64_t bytes_after_offset = 0;
    uint64_t rows_bytes = 0;
    if (region->imageExtent.height > 1 &&
        !dn_vk_mul_u64(row_pitch, region->imageExtent.height - 1U, &rows_bytes))
        return false;
    if (!dn_vk_add_u64(rows_bytes, tight_row, &bytes_after_offset))
        return false;
    if (!dn_vk_buffer_bound_range(buffer, region->bufferOffset, bytes_after_offset,
                                  buffer_usage))
        return false;
    if (out_row_pitch)
        *out_row_pitch = region->bufferRowLength == 0 ? 0U : (uint32_t)row_pitch;
    return true;
}

void vkCmdCopyBuffer(VkCommandBuffer handle, VkBuffer src_handle, VkBuffer dst_handle,
                     uint32_t regionCount, const VkBufferCopy *pRegions)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_buffer *src = dn_vk_get_buffer(src_handle, cb->device);
    struct dn_vk_buffer *dst = dn_vk_get_buffer(dst_handle, cb->device);
    if (!src || !dst || (regionCount > 0 && !pRegions)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferCopy *region = &pRegions[i];
        if (!dn_vk_buffer_bound_range(src, region->srcOffset, region->size,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
            !dn_vk_buffer_bound_range(dst, region->dstOffset, region->size,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        if (src->memory == dst->memory) {
            VkDeviceSize src_abs = src->memory_offset + region->srcOffset;
            VkDeviceSize dst_abs = dst->memory_offset + region->dstOffset;
            if (region->size > 0 && src_abs < dst_abs + region->size &&
                dst_abs < src_abs + region->size) {
                dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
                return;
            }
        }
        struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_COPY_BUFFER);
        if (!command)
            return;
        command->src_buffer = src;
        command->dst_buffer = dst;
        command->src_offset = region->srcOffset;
        command->dst_offset = region->dstOffset;
        command->size = region->size;
        dn_vk_retain(&src->head);
        if (dst != src)
            dn_vk_retain(&dst->head);
    }
}

void vkCmdFillBuffer(VkCommandBuffer handle, VkBuffer dst_handle,
                     VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_buffer *dst = dn_vk_get_buffer(dst_handle, cb->device);
    if (!dst) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    VkDeviceSize effective = size;
    if (size == VK_WHOLE_SIZE) {
        if (dstOffset > dst->size) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        effective = (dst->size - dstOffset) & ~(VkDeviceSize)3ULL;
    }
    if (dstOffset % 4U != 0 || effective % 4U != 0 ||
        !dn_vk_buffer_bound_range(dst, dstOffset, effective,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_FILL_BUFFER);
    if (!command)
        return;
    command->dst_buffer = dst;
    command->dst_offset = dstOffset;
    command->size = effective;
    command->data = data;
    dn_vk_retain(&dst->head);
}

void vkCmdUpdateBuffer(VkCommandBuffer handle, VkBuffer dst_handle,
                       VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_buffer *dst = dn_vk_get_buffer(dst_handle, cb->device);
    if (!dst || !pData || dataSize == 0 || dataSize > 65536U ||
        dstOffset % 4U != 0 || dataSize % 4U != 0 ||
        !dst->memory || !dst->memory->cpu_ptr ||
        !dn_vk_buffer_bound_range(dst, dstOffset, dataSize,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    void *copy = malloc((size_t)dataSize);
    if (!copy) {
        dn_vk_cmd_fail(cb, VK_ERROR_OUT_OF_HOST_MEMORY);
        return;
    }
    memcpy(copy, pData, (size_t)dataSize);
    struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_UPDATE_BUFFER);
    if (!command) {
        free(copy);
        return;
    }
    command->dst_buffer = dst;
    command->dst_offset = dstOffset;
    command->size = dataSize;
    command->owned_data = copy;
    dn_vk_retain(&dst->head);
}

void vkCmdClearColorImage(VkCommandBuffer handle, VkImage image_handle,
                          VkImageLayout imageLayout, const VkClearColorValue *pColor,
                          uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_image *image = dn_vk_get_image(image_handle, cb->device);
    if (!image || !image->memory || !pColor || rangeCount == 0 || !pRanges ||
        (imageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         imageLayout != VK_IMAGE_LAYOUT_GENERAL)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < rangeCount; ++i) {
        if (!dn_vk_image_range_ok(&pRanges[i])) {
            dn_vk_cmd_fail(cb, VK_ERROR_FORMAT_NOT_SUPPORTED);
            return;
        }
    }
    struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_CLEAR_COLOR_IMAGE);
    if (!command)
        return;
    command->image = image;
    command->dst_image = image;
    command->layout = imageLayout;
    command->clear_color = *pColor;
    dn_vk_retain(&image->head);
}

void vkCmdCopyBufferToImage(VkCommandBuffer handle, VkBuffer src_handle,
                            VkImage dst_handle, VkImageLayout dstImageLayout,
                            uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_buffer *src = dn_vk_get_buffer(src_handle, cb->device);
    struct dn_vk_image *dst = dn_vk_get_image(dst_handle, cb->device);
    if (!src || !dst || (regionCount > 0 && !pRegions) ||
        (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
        (dstImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         dstImageLayout != VK_IMAGE_LAYOUT_GENERAL)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy *region = &pRegions[i];
        uint32_t row_pitch = 0;
        if (!dn_vk_buffer_image_bytes_ok(src, dst, region,
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &row_pitch)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_COPY_BUFFER_TO_IMAGE);
        if (!command)
            return;
        command->src_buffer = src;
        command->dst_image = dst;
        command->src_offset = region->bufferOffset;
        command->buffer_row_pitch_bytes = row_pitch;
        command->dst_x = (uint32_t)region->imageOffset.x;
        command->dst_y = (uint32_t)region->imageOffset.y;
        command->width = region->imageExtent.width;
        command->height = region->imageExtent.height;
        command->layout = dstImageLayout;
        dn_vk_retain(&src->head);
        dn_vk_retain(&dst->head);
    }
}

void vkCmdCopyImageToBuffer(VkCommandBuffer handle, VkImage src_handle,
                            VkImageLayout srcImageLayout, VkBuffer dst_handle,
                            uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_image *src = dn_vk_get_image(src_handle, cb->device);
    struct dn_vk_buffer *dst = dn_vk_get_buffer(dst_handle, cb->device);
    if (!src || !dst || (regionCount > 0 && !pRegions) ||
        (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        (srcImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         srcImageLayout != VK_IMAGE_LAYOUT_GENERAL)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy *region = &pRegions[i];
        uint32_t row_pitch = 0;
        if (!dn_vk_buffer_image_bytes_ok(dst, src, region,
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, &row_pitch)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_COPY_IMAGE_TO_BUFFER);
        if (!command)
            return;
        command->src_image = src;
        command->dst_buffer = dst;
        command->dst_offset = region->bufferOffset;
        command->buffer_row_pitch_bytes = row_pitch;
        command->src_x = (uint32_t)region->imageOffset.x;
        command->src_y = (uint32_t)region->imageOffset.y;
        command->width = region->imageExtent.width;
        command->height = region->imageExtent.height;
        command->layout = srcImageLayout;
        dn_vk_retain(&src->head);
        dn_vk_retain(&dst->head);
    }
}

void vkCmdCopyImage(VkCommandBuffer handle, VkImage src_handle,
                    VkImageLayout srcImageLayout, VkImage dst_handle,
                    VkImageLayout dstImageLayout, uint32_t regionCount,
                    const VkImageCopy *pRegions)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    struct dn_vk_image *src = dn_vk_get_image(src_handle, cb->device);
    struct dn_vk_image *dst = dn_vk_get_image(dst_handle, cb->device);
    if (!src || !dst || (regionCount > 0 && !pRegions) || src->format != dst->format ||
        (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 ||
        (srcImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         srcImageLayout != VK_IMAGE_LAYOUT_GENERAL) ||
        (dstImageLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         dstImageLayout != VK_IMAGE_LAYOUT_GENERAL)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkImageCopy *region = &pRegions[i];
        if (!dn_vk_image_subresource_ok(region->srcSubresource.aspectMask,
                                        region->srcSubresource.mipLevel,
                                        region->srcSubresource.baseArrayLayer,
                                        region->srcSubresource.layerCount, false) ||
            !dn_vk_image_subresource_ok(region->dstSubresource.aspectMask,
                                        region->dstSubresource.mipLevel,
                                        region->dstSubresource.baseArrayLayer,
                                        region->dstSubresource.layerCount, false) ||
            !dn_vk_image_rect_ok(src, region->srcOffset, region->extent) ||
            !dn_vk_image_rect_ok(dst, region->dstOffset, region->extent)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_COPY_IMAGE);
        if (!command)
            return;
        command->src_image = src;
        command->dst_image = dst;
        command->src_x = (uint32_t)region->srcOffset.x;
        command->src_y = (uint32_t)region->srcOffset.y;
        command->dst_x = (uint32_t)region->dstOffset.x;
        command->dst_y = (uint32_t)region->dstOffset.y;
        command->width = region->extent.width;
        command->height = region->extent.height;
        command->layout = dstImageLayout;
        command->old_layout = srcImageLayout;
        dn_vk_retain(&src->head);
        if (dst != src)
            dn_vk_retain(&dst->head);
    }
}

void vkCmdPipelineBarrier(VkCommandBuffer handle,
                          VkPipelineStageFlags srcStageMask,
                          VkPipelineStageFlags dstStageMask,
                          VkDependencyFlags dependencyFlags,
                          uint32_t memoryBarrierCount,
                          const VkMemoryBarrier *pMemoryBarriers,
                          uint32_t bufferMemoryBarrierCount,
                          const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                          uint32_t imageMemoryBarrierCount,
                          const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    struct dn_vk_command_buffer *cb = dn_vk_get_command_buffer(handle, NULL);
    if (!dn_vk_cmd_recording(cb))
        return;
    if (srcStageMask == 0 || dstStageMask == 0 ||
        (srcStageMask & ~DN_VK_ALL_PIPELINE_STAGES) != 0 ||
        (dstStageMask & ~DN_VK_ALL_PIPELINE_STAGES) != 0 ||
        (dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0 ||
        (memoryBarrierCount > 0 && !pMemoryBarriers) ||
        (bufferMemoryBarrierCount > 0 && !pBufferMemoryBarriers) ||
        (imageMemoryBarrierCount > 0 && !pImageMemoryBarriers)) {
        dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    for (uint32_t i = 0; i < memoryBarrierCount; ++i) {
        const VkMemoryBarrier *barrier = &pMemoryBarriers[i];
        if (barrier->sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || barrier->pNext ||
            (barrier->srcAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0 ||
            (barrier->dstAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
    }
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier *barrier = &pBufferMemoryBarriers[i];
        struct dn_vk_buffer *buffer = dn_vk_get_buffer(barrier->buffer, cb->device);
        if (barrier->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || barrier->pNext ||
            !buffer ||
            (barrier->srcAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0 ||
            (barrier->dstAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0 ||
            (barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             barrier->srcQueueFamilyIndex != 0) ||
            (barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             barrier->dstQueueFamilyIndex != 0)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        VkDeviceSize effective = barrier->size == VK_WHOLE_SIZE ?
            (barrier->offset <= buffer->size ? buffer->size - barrier->offset : 0) : barrier->size;
        if (!dn_vk_range_ok(buffer->size, barrier->offset, effective)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
    }
    for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier *barrier = &pImageMemoryBarriers[i];
        struct dn_vk_image *image = dn_vk_get_image(barrier->image, cb->device);
        if (barrier->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || barrier->pNext ||
            !image || !image->memory ||
            (barrier->srcAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0 ||
            (barrier->dstAccessMask & ~DN_VK_ALL_ACCESS_FLAGS) != 0 ||
            !dn_vk_layout_supported(barrier->oldLayout) ||
            !dn_vk_layout_supported(barrier->newLayout) ||
            (barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             barrier->srcQueueFamilyIndex != 0) ||
            (barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
             barrier->dstQueueFamilyIndex != 0) ||
            !dn_vk_image_range_ok(&barrier->subresourceRange)) {
            dn_vk_cmd_fail(cb, VK_ERROR_INITIALIZATION_FAILED);
            return;
        }
        struct dn_vk_command *command = dn_vk_cmd_append(cb, DN_VK_CMD_IMAGE_BARRIER);
        if (!command)
            return;
        command->image = image;
        command->old_layout = barrier->oldLayout;
        command->new_layout = barrier->newLayout;
        dn_vk_retain(&image->head);
    }
}

/* ------------------------------------------------------------------ */
/* Submission                                                          */
/* ------------------------------------------------------------------ */

static bool dn_vk_execute_command(struct dn_vk_device *device,
                                  const struct dn_vk_command *command)
{
    int rc = 0;
    switch (command->kind) {
    case DN_VK_CMD_COPY_BUFFER:
        rc = dn_gpu_backend_copy_buffer(device->backend,
                                        command->src_buffer->memory->backend_buffer,
                                        command->src_buffer->memory_offset + command->src_offset,
                                        command->dst_buffer->memory->backend_buffer,
                                        command->dst_buffer->memory_offset + command->dst_offset,
                                        command->size);
        break;
    case DN_VK_CMD_FILL_BUFFER:
        rc = dn_gpu_backend_fill_buffer(device->backend,
                                        command->dst_buffer->memory->backend_buffer,
                                        command->dst_buffer->memory_offset + command->dst_offset,
                                        command->size, command->data);
        break;
    case DN_VK_CMD_UPDATE_BUFFER: {
        struct dn_vk_memory *memory = command->dst_buffer->memory;
        if (!memory->cpu_ptr || !command->owned_data)
            return false;
        memcpy((uint8_t *)memory->cpu_ptr + command->dst_buffer->memory_offset +
                   command->dst_offset,
               command->owned_data, (size_t)command->size);
        rc = 0;
        break;
    }
    case DN_VK_CMD_CLEAR_COLOR_IMAGE:
        rc = dn_gpu_backend_clear_color_image(device->backend,
                                              command->image->backend_image,
                                              command->image->backend_format,
                                              command->clear_color.float32[0],
                                              command->clear_color.float32[1],
                                              command->clear_color.float32[2],
                                              command->clear_color.float32[3]);
        if (rc == 0)
            command->image->layout = command->layout;
        break;
    case DN_VK_CMD_COPY_BUFFER_TO_IMAGE:
        rc = dn_gpu_backend_copy_buffer_to_image(device->backend,
                                                 command->src_buffer->memory->backend_buffer,
                                                 command->src_buffer->memory_offset + command->src_offset,
                                                 command->buffer_row_pitch_bytes,
                                                 command->dst_image->backend_image,
                                                 command->dst_x, command->dst_y,
                                                 command->width, command->height);
        if (rc == 0)
            command->dst_image->layout = command->layout;
        break;
    case DN_VK_CMD_COPY_IMAGE_TO_BUFFER:
        rc = dn_gpu_backend_copy_image_to_buffer(device->backend,
                                                 command->src_image->backend_image,
                                                 command->src_x, command->src_y,
                                                 command->width, command->height,
                                                 command->dst_buffer->memory->backend_buffer,
                                                 command->dst_buffer->memory_offset + command->dst_offset,
                                                 command->buffer_row_pitch_bytes);
        if (rc == 0)
            command->src_image->layout = command->layout;
        break;
    case DN_VK_CMD_COPY_IMAGE:
        rc = dn_gpu_backend_copy_image(device->backend,
                                       command->src_image->backend_image,
                                       command->src_x, command->src_y,
                                       command->dst_image->backend_image,
                                       command->dst_x, command->dst_y,
                                       command->width, command->height);
        if (rc == 0) {
            command->src_image->layout = command->old_layout;
            command->dst_image->layout = command->layout;
        }
        break;
    case DN_VK_CMD_IMAGE_BARRIER:
        if (command->old_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            command->image->layout != command->old_layout)
            return false;
        command->image->layout = command->new_layout;
        rc = 0;
        break;
    default:
        return false;
    }
    return rc == 0;
}

static VkResult dn_vk_validate_submit(struct dn_vk_device *device,
                                      const VkSubmitInfo *submit)
{
    if (submit->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO || submit->pNext)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (submit->waitSemaphoreCount > 0 &&
        (!submit->pWaitSemaphores || !submit->pWaitDstStageMask))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (submit->commandBufferCount > 0 && !submit->pCommandBuffers)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (submit->signalSemaphoreCount > 0 && !submit->pSignalSemaphores)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < submit->waitSemaphoreCount; ++i) {
        struct dn_vk_semaphore *semaphore =
            dn_vk_get_semaphore(submit->pWaitSemaphores[i], device);
        if (!semaphore || submit->pWaitDstStageMask[i] == 0 ||
            (submit->pWaitDstStageMask[i] & ~DN_VK_ALL_PIPELINE_STAGES) != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (atomic_load_explicit(&semaphore->signaled, memory_order_acquire) == VK_FALSE)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < submit->commandBufferCount; ++i) {
        struct dn_vk_command_buffer *cb =
            dn_vk_get_command_buffer(submit->pCommandBuffers[i], device);
        if (!cb || cb->state != DN_VK_CB_EXECUTABLE)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < submit->signalSemaphoreCount; ++i) {
        struct dn_vk_semaphore *semaphore =
            dn_vk_get_semaphore(submit->pSignalSemaphores[i], device);
        if (!semaphore)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (atomic_load_explicit(&semaphore->signaled, memory_order_acquire) != VK_FALSE)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult vkQueueSubmit(VkQueue handle, uint32_t submitCount,
                       const VkSubmitInfo *pSubmits, VkFence fence_handle)
{
    struct dn_vk_queue *queue = dn_vk_get_queue(handle);
    if (!queue || (submitCount > 0 && !pSubmits))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct dn_vk_device *device = queue->device;
    VkResult result = dn_vk_device_result(device);
    if (result != VK_SUCCESS)
        return result;
    struct dn_vk_fence *fence = NULL;
    if (fence_handle != VK_NULL_HANDLE) {
        fence = dn_vk_get_fence(fence_handle, device);
        if (!fence || atomic_load_explicit(&fence->signaled, memory_order_acquire) != VK_FALSE)
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    pthread_mutex_lock(&device->lock);
    for (uint32_t i = 0; i < submitCount; ++i) {
        result = dn_vk_validate_submit(device, &pSubmits[i]);
        if (result != VK_SUCCESS) {
            pthread_mutex_unlock(&device->lock);
            return result;
        }
    }

    for (uint32_t s = 0; s < submitCount; ++s) {
        const VkSubmitInfo *submit = &pSubmits[s];
        for (uint32_t i = 0; i < submit->waitSemaphoreCount; ++i) {
            struct dn_vk_semaphore *semaphore =
                dn_vk_get_semaphore(submit->pWaitSemaphores[i], device);
            atomic_store_explicit(&semaphore->signaled, VK_FALSE, memory_order_release);
        }
        for (uint32_t c = 0; c < submit->commandBufferCount; ++c) {
            struct dn_vk_command_buffer *cb =
                dn_vk_get_command_buffer(submit->pCommandBuffers[c], device);
            for (size_t i = 0; i < cb->command_count; ++i) {
                if (!dn_vk_execute_command(device, &cb->commands[i])) {
                    atomic_store_explicit(&device->lost, VK_TRUE, memory_order_release);
                    pthread_mutex_unlock(&device->lock);
                    return VK_ERROR_DEVICE_LOST;
                }
            }
            cb->state = (cb->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ?
                DN_VK_CB_INVALID : DN_VK_CB_EXECUTABLE;
        }
        for (uint32_t i = 0; i < submit->signalSemaphoreCount; ++i) {
            struct dn_vk_semaphore *semaphore =
                dn_vk_get_semaphore(submit->pSignalSemaphores[i], device);
            atomic_store_explicit(&semaphore->signaled, VK_TRUE, memory_order_release);
        }
    }
    pthread_mutex_unlock(&device->lock);

    if (fence)
        atomic_store_explicit(&fence->signaled, VK_TRUE, memory_order_release);
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Symbol discovery                                                    */
/* ------------------------------------------------------------------ */

enum dn_vk_symbol_level {
    DN_VK_LEVEL_GLOBAL = 1U << 0,
    DN_VK_LEVEL_INSTANCE = 1U << 1,
    DN_VK_LEVEL_DEVICE = 1U << 2
};

struct dn_vk_symbol {
    const char *name;
    void *address;
    uint32_t levels;
};

#define DN_VK_SYM(levels, fn) { #fn, (void *)(fn), (levels) }
static const struct dn_vk_symbol dn_vk_symbols[] = {
    DN_VK_SYM(DN_VK_LEVEL_GLOBAL, vkGetInstanceProcAddr),
    DN_VK_SYM(DN_VK_LEVEL_GLOBAL, vkEnumerateInstanceVersion),
    DN_VK_SYM(DN_VK_LEVEL_GLOBAL, vkEnumerateInstanceExtensionProperties),
    DN_VK_SYM(DN_VK_LEVEL_GLOBAL, vkEnumerateInstanceLayerProperties),
    DN_VK_SYM(DN_VK_LEVEL_GLOBAL, vkCreateInstance),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkDestroyInstance),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkEnumeratePhysicalDevices),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceFeatures),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceQueueFamilyProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceMemoryProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceFormatProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkGetPhysicalDeviceImageFormatProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkEnumerateDeviceExtensionProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkEnumerateDeviceLayerProperties),
    DN_VK_SYM(DN_VK_LEVEL_INSTANCE, vkCreateDevice),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkGetDeviceProcAddr),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyDevice),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkGetDeviceQueue),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDeviceWaitIdle),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkQueueWaitIdle),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkQueueSubmit),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkAllocateMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkFreeMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkMapMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkUnmapMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkFlushMappedMemoryRanges),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkInvalidateMappedMemoryRanges),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkGetBufferMemoryRequirements),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkBindBufferMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateImage),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyImage),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkGetImageMemoryRequirements),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkBindImageMemory),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateImageView),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyImageView),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateCommandPool),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyCommandPool),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkResetCommandPool),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkAllocateCommandBuffers),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkFreeCommandBuffers),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkBeginCommandBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkEndCommandBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkResetCommandBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateFence),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroyFence),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkResetFences),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkGetFenceStatus),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkWaitForFences),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCreateSemaphore),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkDestroySemaphore),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdCopyBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdFillBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdUpdateBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdClearColorImage),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdCopyBufferToImage),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdCopyImageToBuffer),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdCopyImage),
    DN_VK_SYM(DN_VK_LEVEL_DEVICE, vkCmdPipelineBarrier)
};
#undef DN_VK_SYM

static const struct dn_vk_symbol *dn_vk_find_symbol(const char *name, uint32_t levels)
{
    if (!name)
        return NULL;
    const size_t count = sizeof(dn_vk_symbols) / sizeof(dn_vk_symbols[0]);
    for (size_t i = 0; i < count; ++i) {
        if ((dn_vk_symbols[i].levels & levels) != 0 &&
            strcmp(name, dn_vk_symbols[i].name) == 0)
            return &dn_vk_symbols[i];
    }
    return NULL;
}

void *dn_vulkan_lookup_symbol(const char *name)
{
    const struct dn_vk_symbol *symbol = dn_vk_find_symbol(
        name, DN_VK_LEVEL_GLOBAL | DN_VK_LEVEL_INSTANCE | DN_VK_LEVEL_DEVICE);
    return symbol ? symbol->address : NULL;
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance handle, const char *pName)
{
    if (!pName)
        return NULL;
    if (handle == VK_NULL_HANDLE) {
        const struct dn_vk_symbol *symbol = dn_vk_find_symbol(pName, DN_VK_LEVEL_GLOBAL);
        return symbol ? (PFN_vkVoidFunction)symbol->address : NULL;
    }
    if (!dn_vk_get_instance(handle))
        return NULL;
    const struct dn_vk_symbol *symbol = dn_vk_find_symbol(
        pName, DN_VK_LEVEL_GLOBAL | DN_VK_LEVEL_INSTANCE | DN_VK_LEVEL_DEVICE);
    return symbol ? (PFN_vkVoidFunction)symbol->address : NULL;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice handle, const char *pName)
{
    if (!pName || !dn_vk_get_device(handle))
        return NULL;
    const struct dn_vk_symbol *symbol = dn_vk_find_symbol(pName, DN_VK_LEVEL_DEVICE);
    return symbol ? (PFN_vkVoidFunction)symbol->address : NULL;
}

/* ------------------------------------------------------------------ */
/* Runtime self-test (used by the app and by host tests)               */
/* ------------------------------------------------------------------ */

struct dn_vk_report {
    char *out;
    size_t out_len;
    size_t used;
};

static void dn_vk_report_line(struct dn_vk_report *report, const char *fmt, ...)
{
    if (!report->out || report->out_len == 0 || report->used >= report->out_len)
        return;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(report->out + report->used,
                            report->out_len - report->used, fmt, args);
    va_end(args);
    if (written < 0)
        return;
    size_t available = report->out_len - report->used;
    report->used += (size_t)written < available ? (size_t)written : available - 1;
    if (report->used + 1 < report->out_len) {
        report->out[report->used++] = '\n';
        report->out[report->used] = '\0';
    }
}

static bool dn_vk_selftest_expect(struct dn_vk_report *report, VkResult got,
                                  const char *what, bool *ok)
{
    bool pass = got == VK_SUCCESS;
    dn_vk_report_line(report, "%s: %s (%d)", what, pass ? "OK" : "FAIL", (int)got);
    *ok = *ok && pass;
    return pass;
}

static VkResult dn_vk_selftest_make_buffer(VkDevice device, VkDeviceSize size,
                                           VkBufferUsageFlags usage,
                                           VkBuffer *buffer, VkDeviceMemory *memory)
{
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_info;
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &buffer_info, NULL, buffer);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    VkMemoryAllocateInfo alloc_info;
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = 0;
    result = vkAllocateMemory(device, &alloc_info, NULL, memory);
    if (result != VK_SUCCESS)
        return result;
    return vkBindBufferMemory(device, *buffer, *memory, 0);
}

int dn_vulkan_run_selftest(char *out, size_t out_len)
{
    struct dn_vk_report report;
    report.out = out;
    report.out_len = out_len;
    report.used = 0;
    if (out && out_len)
        out[0] = '\0';

    bool ok = true;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffers[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
    VkDeviceMemory readback_mem = VK_NULL_HANDLE, image_mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "iDroid Vulkan self-test";
    app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    app.pEngineName = "DroidNative";
    app.engineVersion = VK_MAKE_VERSION(0, 3, 0);
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (!dn_vk_selftest_expect(&report, vkCreateInstance(&instance_info, NULL, &instance),
                               "vkCreateInstance", &ok))
        goto done;

    uint32_t physical_count = 0;
    if (!dn_vk_selftest_expect(&report, vkEnumeratePhysicalDevices(instance, &physical_count, NULL),
                               "vkEnumeratePhysicalDevices count", &ok))
        goto done;
    dn_vk_report_line(&report, "physical devices: %u", physical_count);
    if (physical_count != 1) {
        ok = false;
        goto done;
    }
    physical_count = 1;
    if (!dn_vk_selftest_expect(&report, vkEnumeratePhysicalDevices(instance, &physical_count, &physical),
                               "vkEnumeratePhysicalDevices fetch", &ok))
        goto done;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    dn_vk_report_line(&report, "GPU: %s (api %u.%u, max2D %u)", props.deviceName,
                      VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                      props.limits.maxImageDimension2D);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info;
    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info;
    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (!dn_vk_selftest_expect(&report, vkCreateDevice(physical, &device_info, NULL, &device),
                               "vkCreateDevice", &ok))
        goto done;
    vkGetDeviceQueue(device, 0, 0, &queue);
    dn_vk_report_line(&report, "queue: %s", queue ? "OK" : "FAIL");
    ok = ok && queue != VK_NULL_HANDLE;
    if (!queue)
        goto done;

    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0;
    if (!dn_vk_selftest_expect(&report, vkCreateCommandPool(device, &pool_info, NULL, &pool),
                               "vkCreateCommandPool", &ok))
        goto done;
    VkCommandBufferAllocateInfo alloc_cb;
    memset(&alloc_cb, 0, sizeof(alloc_cb));
    alloc_cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_cb.commandPool = pool;
    alloc_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_cb.commandBufferCount = 2;
    if (!dn_vk_selftest_expect(&report, vkAllocateCommandBuffers(device, &alloc_cb, command_buffers),
                               "vkAllocateCommandBuffers", &ok))
        goto done;
    VkFenceCreateInfo fence_info;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!dn_vk_selftest_expect(&report, vkCreateFence(device, &fence_info, NULL, &fence),
                               "vkCreateFence", &ok))
        goto done;

    if (!dn_vk_selftest_expect(&report,
                               dn_vk_selftest_make_buffer(device, 64,
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                          &src, &src_mem),
                               "create src buffer", &ok) ||
        !dn_vk_selftest_expect(&report,
                               dn_vk_selftest_make_buffer(device, 64,
                                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                          &dst, &dst_mem),
                               "create dst buffer", &ok))
        goto done;

    uint8_t *mapped = NULL;
    if (!dn_vk_selftest_expect(&report, vkMapMemory(device, src_mem, 0, VK_WHOLE_SIZE, 0,
                                                    (void **)&mapped),
                               "map src buffer", &ok))
        goto done;
    for (uint32_t i = 0; i < 64; ++i)
        mapped[i] = (uint8_t)(i * 3U + 7U);
    vkUnmapMemory(device, src_mem);

    VkCommandBufferBeginInfo begin;
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!dn_vk_selftest_expect(&report, vkBeginCommandBuffer(command_buffers[0], &begin),
                               "begin buffer commands", &ok))
        goto done;
    VkBufferCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.srcOffset = 0;
    copy.dstOffset = 0;
    copy.size = 64;
    vkCmdCopyBuffer(command_buffers[0], src, dst, 1, &copy);
    vkCmdFillBuffer(command_buffers[0], dst, 8, 16, 0x11223344U);
    if (!dn_vk_selftest_expect(&report, vkEndCommandBuffer(command_buffers[0]),
                               "end buffer commands", &ok))
        goto done;
    VkSubmitInfo submit;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffers[0];
    if (!dn_vk_selftest_expect(&report, vkQueueSubmit(queue, 1, &submit, fence),
                               "submit buffer copy+fill", &ok) ||
        !dn_vk_selftest_expect(&report, vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
                               "wait fence", &ok))
        goto done;
    if (!dn_vk_selftest_expect(&report, vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0,
                                                    (void **)&mapped),
                               "map dst buffer", &ok))
        goto done;
    bool buffer_ok = mapped[0] == 7 && mapped[7] == 28 && mapped[24] == 79 &&
                     mapped[8] == 0x44 && mapped[9] == 0x33 &&
                     mapped[10] == 0x22 && mapped[11] == 0x11;
    dn_vk_report_line(&report, "buffer copy+fill bytes: %s", buffer_ok ? "OK" : "FAIL");
    ok = ok && buffer_ok;
    vkUnmapMemory(device, dst_mem);

    VkImageCreateInfo image_info;
    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = 2;
    image_info.extent.height = 2;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!dn_vk_selftest_expect(&report, vkCreateImage(device, &image_info, NULL, &image),
                               "vkCreateImage RGBA8", &ok))
        goto done;
    VkMemoryRequirements image_req;
    vkGetImageMemoryRequirements(device, image, &image_req);
    VkMemoryAllocateInfo image_alloc;
    memset(&image_alloc, 0, sizeof(image_alloc));
    image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc.allocationSize = image_req.size;
    image_alloc.memoryTypeIndex = 0;
    if (!dn_vk_selftest_expect(&report, vkAllocateMemory(device, &image_alloc, NULL, &image_mem),
                               "allocate image memory", &ok) ||
        !dn_vk_selftest_expect(&report, vkBindImageMemory(device, image, image_mem, 0),
                               "bind image memory", &ok))
        goto done;
    VkImageViewCreateInfo view_info;
    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (!dn_vk_selftest_expect(&report, vkCreateImageView(device, &view_info, NULL, &view),
                               "vkCreateImageView", &ok) ||
        !dn_vk_selftest_expect(&report,
                               dn_vk_selftest_make_buffer(device, 16,
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                          &readback, &readback_mem),
                               "create readback buffer", &ok) ||
        !dn_vk_selftest_expect(&report, vkResetFences(device, 1, &fence),
                               "reset fence", &ok))
        goto done;

    if (!dn_vk_selftest_expect(&report, vkBeginCommandBuffer(command_buffers[1], &begin),
                               "begin image commands", &ok))
        goto done;
    VkImageMemoryBarrier barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_buffers[1], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkClearColorValue clear;
    clear.float32[0] = 0.25f;
    clear.float32[1] = 0.5f;
    clear.float32[2] = 1.0f;
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange clear_range;
    memset(&clear_range, 0, sizeof(clear_range));
    clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_range.levelCount = 1;
    clear_range.layerCount = 1;
    vkCmdClearColorImage(command_buffers[1], image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &clear_range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command_buffers[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = 2;
    region.imageExtent.height = 2;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(command_buffers[1], image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &region);
    if (!dn_vk_selftest_expect(&report, vkEndCommandBuffer(command_buffers[1]),
                               "end image commands", &ok))
        goto done;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffers[1];
    if (!dn_vk_selftest_expect(&report, vkQueueSubmit(queue, 1, &submit, fence),
                               "submit image clear+readback", &ok) ||
        !dn_vk_selftest_expect(&report, vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
                               "wait image fence", &ok) ||
        !dn_vk_selftest_expect(&report, vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0,
                                                    (void **)&mapped),
                               "map readback", &ok))
        goto done;
    bool image_ok = mapped[0] == 64 && mapped[1] == 128 && mapped[2] == 255 && mapped[3] == 255 &&
                    mapped[12] == 64 && mapped[13] == 128 && mapped[14] == 255 && mapped[15] == 255;
    dn_vk_report_line(&report, "image clear/readback pixels: %s", image_ok ? "OK" : "FAIL");
    ok = ok && image_ok;
    vkUnmapMemory(device, readback_mem);

done:
    if (view) vkDestroyImageView(device, view, NULL);
    if (readback) vkDestroyBuffer(device, readback, NULL);
    if (readback_mem) vkFreeMemory(device, readback_mem, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    if (image_mem) vkFreeMemory(device, image_mem, NULL);
    if (src) vkDestroyBuffer(device, src, NULL);
    if (src_mem) vkFreeMemory(device, src_mem, NULL);
    if (dst) vkDestroyBuffer(device, dst, NULL);
    if (dst_mem) vkFreeMemory(device, dst_mem, NULL);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    dn_vk_report_line(&report, "%s - DroidNative Vulkan-on-Metal subset self-test",
                      ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
