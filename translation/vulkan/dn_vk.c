/*
 * Guest-side fixture for iDroid 0.3 (docs/13 native GPU step).
 *
 * This ARM64 ELF library is converted/signed on the Mac by
 * scripts/make-vulkan-native.sh, loaded by dyld, linked by DN's own
 * ELF mini-linker, and then called directly.  Its vk* imports bind to
 * DroidNative's Vulkan-on-Metal driver.  The sequence deliberately
 * exercises the supported 0.3 subset hard enough to be a real gate:
 * buffer copy + 32-bit fill, padded buffer->image upload, image->image
 * copy, layout barriers, image->buffer readback, then BGRA color clear
 * and a byte-exact FNV checksum.
 */

#include "../../app/DroidNative/NativeRuntime/DNGpuVulkan.h"

#include <stddef.h>
#include <stdint.h>

/* Declared manually like the step-2.5 fixtures: some freestanding
 * cross-compile modes used by make-hello-native.sh intentionally have
 * no libc include path. The symbol itself binds to DN's host shim. */
extern void *memset(void *dst, int value, size_t count);

typedef int dn_vk_bool;
#define DN_VK_FALSE 0
#define DN_VK_TRUE 1

static volatile uint32_t dn_vk_checksum_value;
static volatile int dn_vk_last_step_value;

static uint32_t dn_vk_fnv1a32(const uint8_t *bytes, uint32_t count)
{
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static VkResult dn_vk_make_buffer(VkDevice device, VkDeviceSize size,
                                  VkBufferUsageFlags usage,
                                  VkBuffer *buffer, VkDeviceMemory *memory)
{
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    VkBufferCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &info, NULL, buffer);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    VkMemoryAllocateInfo alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = 0;
    result = vkAllocateMemory(device, &alloc, NULL, memory);
    if (result != VK_SUCCESS)
        return result;
    return vkBindBufferMemory(device, *buffer, *memory, 0);
}

static VkResult dn_vk_make_image(VkDevice device, VkImage *image, VkDeviceMemory *memory)
{
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    VkImageCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_B8G8R8A8_UNORM;
    info.extent.width = 4;
    info.extent.height = 3;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(device, &info, NULL, image);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *image, &req);
    VkMemoryAllocateInfo alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = 0;
    result = vkAllocateMemory(device, &alloc, NULL, memory);
    if (result != VK_SUCCESS)
        return result;
    return vkBindImageMemory(device, *image, *memory, 0);
}

static void dn_vk_barrier(VkCommandBuffer cb, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
}

__attribute__((visibility("default")))
uint32_t dn_vk_checksum(void)
{
    return dn_vk_checksum_value;
}

__attribute__((visibility("default")))
int dn_vk_last_step(void)
{
    return dn_vk_last_step_value;
}

__attribute__((visibility("default")))
int dn_vk_run_test(void)
{
    dn_vk_checksum_value = 0;
    dn_vk_last_step_value = 0;
    int step = 0;
    int rc = 0;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cbs[2];
    cbs[0] = VK_NULL_HANDLE;
    cbs[1] = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE, readback_mem = VK_NULL_HANDLE;
    VkImage image_a = VK_NULL_HANDLE, image_b = VK_NULL_HANDLE;
    VkDeviceMemory image_a_mem = VK_NULL_HANDLE, image_b_mem = VK_NULL_HANDLE;
    uint8_t *mapped = NULL;

#define DN_VK_STEP(n) do { step = (n); } while (0)
#define DN_VK_REQUIRE(expr) do { \
        if ((expr) != VK_SUCCESS) { rc = -step; goto done; } \
        dn_vk_last_step_value = step; \
    } while (0)
#define DN_VK_REQUIRE_TRUE(cond) do { \
        if (!(cond)) { rc = -step; goto done; } \
        dn_vk_last_step_value = step; \
    } while (0)

    DN_VK_STEP(1);
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dn_vk fixture";
    app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    DN_VK_REQUIRE(vkCreateInstance(&instance_info, NULL, &instance));

    DN_VK_STEP(2);
    uint32_t physical_count = 0;
    DN_VK_REQUIRE(vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
    DN_VK_REQUIRE_TRUE(physical_count == 1);
    physical_count = 1;
    DN_VK_REQUIRE(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));

    DN_VK_STEP(3);
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
    DN_VK_REQUIRE(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    DN_VK_REQUIRE_TRUE(queue != VK_NULL_HANDLE);

    DN_VK_STEP(4);
    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0;
    DN_VK_REQUIRE(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo alloc_cb;
    memset(&alloc_cb, 0, sizeof(alloc_cb));
    alloc_cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_cb.commandPool = pool;
    alloc_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_cb.commandBufferCount = 2;
    DN_VK_REQUIRE(vkAllocateCommandBuffers(device, &alloc_cb, cbs));
    VkFenceCreateInfo fence_info;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    DN_VK_REQUIRE(vkCreateFence(device, &fence_info, NULL, &fence));

    DN_VK_STEP(5);
    DN_VK_REQUIRE(dn_vk_make_buffer(device, 64,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &src, &src_mem));
    DN_VK_REQUIRE(dn_vk_make_buffer(device, 64,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &dst, &dst_mem));
    DN_VK_REQUIRE(vkMapMemory(device, src_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    for (uint32_t i = 0; i < 64; ++i)
        mapped[i] = (uint8_t)(i * 3U + 7U);
    vkUnmapMemory(device, src_mem);

    DN_VK_STEP(6);
    VkCommandBufferBeginInfo begin;
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DN_VK_REQUIRE(vkBeginCommandBuffer(cbs[0], &begin));
    VkBufferCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.size = 64;
    vkCmdCopyBuffer(cbs[0], src, dst, 1, &copy);
    vkCmdFillBuffer(cbs[0], dst, 8, 16, 0x11223344U);
    DN_VK_REQUIRE(vkEndCommandBuffer(cbs[0]));
    VkSubmitInfo submit;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[0];
    DN_VK_REQUIRE(vkQueueSubmit(queue, 1, &submit, fence));
    DN_VK_REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    DN_VK_REQUIRE(vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    DN_VK_REQUIRE_TRUE(mapped[0] == 7 && mapped[7] == 28 && mapped[24] == 79 &&
                       mapped[8] == 0x44 && mapped[9] == 0x33 &&
                       mapped[10] == 0x22 && mapped[11] == 0x11);
    vkUnmapMemory(device, dst_mem);

    DN_VK_STEP(7);
    DN_VK_REQUIRE(dn_vk_make_buffer(device, 3U * 6U * 4U,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    &staging, &staging_mem));
    DN_VK_REQUIRE(dn_vk_make_buffer(device, 4U * 3U * 4U,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &readback, &readback_mem));
    DN_VK_REQUIRE(dn_vk_make_image(device, &image_a, &image_a_mem));
    DN_VK_REQUIRE(dn_vk_make_image(device, &image_b, &image_b_mem));
    DN_VK_REQUIRE(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    for (uint32_t i = 0; i < 3U * 6U * 4U; ++i)
        mapped[i] = 0xEE;
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint8_t *px = mapped + y * 24U + x * 4U;
            px[0] = (uint8_t)(x * 10U + y);
            px[1] = (uint8_t)(100U + x);
            px[2] = (uint8_t)(200U + y);
            px[3] = 255;
        }
    }
    vkUnmapMemory(device, staging_mem);

    DN_VK_STEP(8);
    DN_VK_REQUIRE(vkResetFences(device, 1, &fence));
    DN_VK_REQUIRE(vkBeginCommandBuffer(cbs[1], &begin));
    dn_vk_barrier(cbs[1], image_a, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy upload;
    memset(&upload, 0, sizeof(upload));
    upload.bufferRowLength = 6;
    upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload.imageSubresource.layerCount = 1;
    upload.imageExtent.width = 4;
    upload.imageExtent.height = 3;
    upload.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cbs[1], staging, image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &upload);
    dn_vk_barrier(cbs[1], image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    dn_vk_barrier(cbs[1], image_b, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy image_copy;
    memset(&image_copy, 0, sizeof(image_copy));
    image_copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_copy.srcSubresource.layerCount = 1;
    image_copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_copy.dstSubresource.layerCount = 1;
    image_copy.extent.width = 4;
    image_copy.extent.height = 3;
    image_copy.extent.depth = 1;
    vkCmdCopyImage(cbs[1], image_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   image_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);
    dn_vk_barrier(cbs[1], image_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy download;
    memset(&download, 0, sizeof(download));
    download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    download.imageSubresource.layerCount = 1;
    download.imageExtent.width = 4;
    download.imageExtent.height = 3;
    download.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cbs[1], image_b, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &download);
    DN_VK_REQUIRE(vkEndCommandBuffer(cbs[1]));
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[1];
    DN_VK_REQUIRE(vkQueueSubmit(queue, 1, &submit, fence));
    DN_VK_REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    DN_VK_REQUIRE(vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    dn_vk_checksum_value = dn_vk_fnv1a32(mapped, 4U * 3U * 4U);
    DN_VK_REQUIRE_TRUE(dn_vk_checksum_value == 0xEEF103F9U);
    vkUnmapMemory(device, readback_mem);

    DN_VK_STEP(9);
    DN_VK_REQUIRE(vkResetCommandPool(device, pool, 0));
    DN_VK_REQUIRE(vkResetFences(device, 1, &fence));
    DN_VK_REQUIRE(vkBeginCommandBuffer(cbs[0], &begin));
    dn_vk_barrier(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
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
    vkCmdClearColorImage(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &clear_range);
    dn_vk_barrier(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &download);
    DN_VK_REQUIRE(vkEndCommandBuffer(cbs[0]));
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[0];
    DN_VK_REQUIRE(vkQueueSubmit(queue, 1, &submit, fence));
    DN_VK_REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    DN_VK_REQUIRE(vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    dn_vk_checksum_value = dn_vk_fnv1a32(mapped, 4U * 3U * 4U);
    DN_VK_REQUIRE_TRUE(dn_vk_checksum_value == 0x1DE4992DU);
    vkUnmapMemory(device, readback_mem);

    dn_vk_last_step_value = 10;
    rc = 0;

done:
    if (image_b != VK_NULL_HANDLE) vkDestroyImage(device, image_b, NULL);
    if (image_b_mem != VK_NULL_HANDLE) vkFreeMemory(device, image_b_mem, NULL);
    if (image_a != VK_NULL_HANDLE) vkDestroyImage(device, image_a, NULL);
    if (image_a_mem != VK_NULL_HANDLE) vkFreeMemory(device, image_a_mem, NULL);
    if (readback != VK_NULL_HANDLE) vkDestroyBuffer(device, readback, NULL);
    if (readback_mem != VK_NULL_HANDLE) vkFreeMemory(device, readback_mem, NULL);
    if (staging != VK_NULL_HANDLE) vkDestroyBuffer(device, staging, NULL);
    if (staging_mem != VK_NULL_HANDLE) vkFreeMemory(device, staging_mem, NULL);
    if (dst != VK_NULL_HANDLE) vkDestroyBuffer(device, dst, NULL);
    if (dst_mem != VK_NULL_HANDLE) vkFreeMemory(device, dst_mem, NULL);
    if (src != VK_NULL_HANDLE) vkDestroyBuffer(device, src, NULL);
    if (src_mem != VK_NULL_HANDLE) vkFreeMemory(device, src_mem, NULL);
    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, NULL);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, NULL);
    if (device != VK_NULL_HANDLE) vkDestroyDevice(device, NULL);
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, NULL);
    return rc;

#undef DN_VK_REQUIRE_TRUE
#undef DN_VK_REQUIRE
#undef DN_VK_STEP
}
