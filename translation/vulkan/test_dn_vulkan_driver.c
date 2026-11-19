#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "DNGpuVulkan.h"
#include "DNNativeShims.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        failures++; \
    } \
} while (0)

#define CHECK_VK(expr, msg) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, (msg), (int)_r); \
        failures++; \
    } \
} while (0)

static uint32_t fnv1a32(const uint8_t *bytes, size_t count)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static VkResult make_buffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
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
    if (req.memoryTypeBits != 1U)
        return VK_ERROR_INITIALIZATION_FAILED;
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

static VkResult make_image(VkDevice device, VkFormat format, uint32_t width, uint32_t height,
                           VkImage *image, VkDeviceMemory *memory)
{
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    VkImageCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
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

static VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout old_layout,
                                          VkImageLayout new_layout,
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
    return barrier;
}

int main(void)
{
    /* ABI guardrails for the subset we expose to official-header guests. */
    CHECK(VK_FORMAT_R8_UNORM == 9 && VK_FORMAT_R8G8B8A8_UNORM == 37 &&
          VK_FORMAT_B8G8R8A8_UNORM == 44 && VK_FORMAT_B8G8R8A8_SRGB == 50,
          "Vulkan format enum values match Khronos ABI");
    CHECK(sizeof(VkPhysicalDeviceProperties) == 824 &&
          sizeof(VkPhysicalDeviceLimits) == 504 &&
          sizeof(VkPhysicalDeviceFeatures) == 220,
          "physical-device property layouts match Vulkan 1.0");
    CHECK(sizeof(VkDeviceCreateInfo) == 72 && sizeof(VkSubmitInfo) == 72 &&
          sizeof(VkBufferCreateInfo) == 56 && sizeof(VkImageCreateInfo) == 88,
          "create/submit structure sizes match Vulkan 1.0");
    CHECK(sizeof(VkBufferImageCopy) == 56 && sizeof(VkImageCopy) == 68 &&
          sizeof(VkImageMemoryBarrier) == 72,
          "command payload sizes match Vulkan 1.0");

    /* Symbol surface: supported subset binds; shader/draw/swapchain do not. */
    CHECK(dn_native_shim_lookup("vkCreateInstance") != NULL, "vkCreateInstance binds through DN shims");
    CHECK(dn_native_shim_lookup("vkCmdCopyBuffer") != NULL, "vkCmdCopyBuffer binds through DN shims");
    CHECK(dn_native_shim_lookup("vkCreateGraphicsPipelines") == NULL, "graphics pipelines fail closed");
    CHECK(dn_native_shim_lookup("vkCmdDraw") == NULL, "draw fails closed");
    CHECK(dn_native_shim_lookup("vkCreateSwapchainKHR") == NULL, "swapchain fails closed");
    CHECK(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") != NULL, "global lookup with NULL instance");
    CHECK(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice") == NULL, "device function hidden with NULL instance");

    char selftest[8192];
    int selftest_rc = dn_vulkan_run_selftest(selftest, sizeof(selftest));
    if (selftest_rc != 0)
        fprintf(stderr, "selftest output:\n%s\n", selftest);
    CHECK(selftest_rc == 0, "dn_vulkan_run_selftest passes");
    CHECK(strstr(selftest, "PASS - DroidNative Vulkan-on-Metal subset self-test") != NULL,
          "selftest prints PASS");

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "DN Vulkan host test";
    app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    CHECK_VK(vkCreateInstance(&instance_info, NULL, &instance), "vkCreateInstance");

    uint32_t extension_count = 123;
    CHECK_VK(vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL),
             "enumerate instance extensions");
    CHECK(extension_count == 0, "no instance extensions advertised");
    CHECK(vkEnumerateInstanceExtensionProperties("missing", &extension_count, NULL) ==
          VK_ERROR_LAYER_NOT_PRESENT, "unknown layer rejected");

    uint32_t physical_count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_count, NULL), "physical count");
    CHECK(physical_count == 1, "one Apple GPU physical device");
    VkPhysicalDevice physical_array[1] = { VK_NULL_HANDLE };
    uint32_t zero_capacity = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &zero_capacity, physical_array) == VK_INCOMPLETE,
          "zero capacity enumeration is incomplete");
    physical_count = 1;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &physical_count, physical_array), "physical fetch");
    VkPhysicalDevice physical = physical_array[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    CHECK(props.apiVersion == VK_API_VERSION_1_0, "API version 1.0");
    CHECK(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, "integrated GPU");
    CHECK(props.limits.maxImageDimension2D == 4096, "conservative image limit");
    CHECK(strstr(props.deviceName, "DroidNative Vulkan-on-Metal") != NULL, "device name");

    VkFormatProperties format_props;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B8G8R8A8_UNORM, &format_props);
    CHECK((format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0,
          "BGRA8 transfer dst supported");
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B8G8R8A8_SRGB, &format_props);
    CHECK(format_props.optimalTilingFeatures == 0, "BGRA8 SRGB intentionally unsupported in 0.3");
    VkImageFormatProperties image_format_props;
    CHECK_VK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                                      VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                                                      &image_format_props),
             "RGBA8 image format properties");
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                                   VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                                   &image_format_props) == VK_ERROR_FORMAT_NOT_SUPPORTED,
          "sampled usage fails closed until shader path exists");

    float priority = 0.5f;
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

    VkPhysicalDeviceFeatures enabled_features;
    memset(&enabled_features, 0, sizeof(enabled_features));
    enabled_features.geometryShader = VK_TRUE;
    VkDeviceCreateInfo bad_device_info = device_info;
    bad_device_info.pEnabledFeatures = &enabled_features;
    VkDevice bad_device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &bad_device_info, NULL, &bad_device) == VK_ERROR_FEATURE_NOT_PRESENT,
          "unsupported feature rejected");
    const char *extension_name = "VK_KHR_swapchain";
    VkDeviceCreateInfo ext_device_info = device_info;
    ext_device_info.enabledExtensionCount = 1;
    ext_device_info.ppEnabledExtensionNames = &extension_name;
    CHECK(vkCreateDevice(physical, &ext_device_info, NULL, &bad_device) == VK_ERROR_EXTENSION_NOT_PRESENT,
          "unsupported extension rejected");

    VkDevice device = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDevice(physical, &device_info, NULL, &device), "vkCreateDevice");
    CHECK(vkGetDeviceProcAddr(device, "vkQueueSubmit") != NULL, "device proc addr");
    CHECK(vkGetDeviceProcAddr(device, "vkCreateInstance") == NULL, "global proc hidden from device lookup");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "queue fetched");

    VkCommandPoolCreateInfo pool_info;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK_VK(vkCreateCommandPool(device, &pool_info, NULL, &pool), "create command pool");
    VkCommandBufferAllocateInfo alloc_cb;
    memset(&alloc_cb, 0, sizeof(alloc_cb));
    alloc_cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_cb.commandPool = pool;
    alloc_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_cb.commandBufferCount = 3;
    VkCommandBuffer cbs[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    CHECK_VK(vkAllocateCommandBuffers(device, &alloc_cb, cbs), "allocate command buffers");

    VkFenceCreateInfo fence_info;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VkFence fence2 = VK_NULL_HANDLE;
    CHECK_VK(vkCreateFence(device, &fence_info, NULL, &fence), "create fence");
    CHECK_VK(vkCreateFence(device, &fence_info, NULL, &fence2), "create fence 2");
    VkSemaphoreCreateInfo semaphore_info;
    memset(&semaphore_info, 0, sizeof(semaphore_info));
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore_a = VK_NULL_HANDLE;
    VkSemaphore semaphore_b = VK_NULL_HANDLE;
    CHECK_VK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore_a), "create semaphore A");
    CHECK_VK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore_b), "create semaphore B");

    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE, unbound = VK_NULL_HANDLE;
    VkDeviceMemory src_mem = VK_NULL_HANDLE, dst_mem = VK_NULL_HANDLE;
    CHECK_VK(make_buffer(device, 128, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &src, &src_mem), "make src");
    CHECK_VK(make_buffer(device, 128, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &dst, &dst_mem), "make dst");
    VkBufferCreateInfo unbound_info;
    memset(&unbound_info, 0, sizeof(unbound_info));
    unbound_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    unbound_info.size = 16;
    unbound_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    CHECK_VK(vkCreateBuffer(device, &unbound_info, NULL, &unbound), "make unbound buffer");

    VkCommandBufferBeginInfo begin;
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK_VK(vkBeginCommandBuffer(cbs[2], &begin), "begin invalid command buffer");
    VkBufferCopy invalid_copy;
    memset(&invalid_copy, 0, sizeof(invalid_copy));
    invalid_copy.size = 4;
    vkCmdCopyBuffer(cbs[2], unbound, dst, 1, &invalid_copy);
    CHECK(vkEndCommandBuffer(cbs[2]) != VK_SUCCESS, "recording with unbound buffer fails at end");
    VkSubmitInfo invalid_submit;
    memset(&invalid_submit, 0, sizeof(invalid_submit));
    invalid_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    invalid_submit.commandBufferCount = 1;
    invalid_submit.pCommandBuffers = &cbs[2];
    CHECK(vkQueueSubmit(queue, 1, &invalid_submit, VK_NULL_HANDLE) != VK_SUCCESS,
          "invalid command buffer cannot submit");
    CHECK_VK(vkResetCommandPool(device, pool, 0), "reset command pool after invalid test");

    uint8_t *mapped = NULL;
    CHECK_VK(vkMapMemory(device, src_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "map src");
    for (uint32_t i = 0; i < 128; ++i)
        mapped[i] = (uint8_t)(i * 5U + 3U);
    vkUnmapMemory(device, src_mem);

    CHECK_VK(vkBeginCommandBuffer(cbs[0], &begin), "begin buffer test");
    VkBufferCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.srcOffset = 4;
    copy.dstOffset = 12;
    copy.size = 32;
    vkCmdCopyBuffer(cbs[0], src, dst, 1, &copy);
    vkCmdFillBuffer(cbs[0], dst, 64, 16, 0xA5C33C5AU);
    static const uint8_t update_bytes[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x10, 0x32, 0x54, 0x76 };
    vkCmdUpdateBuffer(cbs[0], dst, 96, sizeof(update_bytes), update_bytes);
    CHECK_VK(vkEndCommandBuffer(cbs[0]), "end buffer test");
    VkSubmitInfo submit;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[0];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore_a;
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence), "submit buffer test");

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo semaphore_submit;
    memset(&semaphore_submit, 0, sizeof(semaphore_submit));
    semaphore_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    semaphore_submit.waitSemaphoreCount = 1;
    semaphore_submit.pWaitSemaphores = &semaphore_a;
    semaphore_submit.pWaitDstStageMask = &wait_stage;
    semaphore_submit.signalSemaphoreCount = 1;
    semaphore_submit.pSignalSemaphores = &semaphore_b;
    CHECK_VK(vkQueueSubmit(queue, 1, &semaphore_submit, fence2), "semaphore-only submit chains A to B");
    CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, 0), "fence signaled");
    CHECK_VK(vkWaitForFences(device, 1, &fence2, VK_TRUE, 0), "fence2 signaled");
    CHECK_VK(vkMapMemory(device, dst_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "map dst");
    CHECK(mapped[12] == (uint8_t)(4U * 5U + 3U), "buffer copy first byte");
    CHECK(mapped[43] == (uint8_t)(35U * 5U + 3U), "buffer copy last byte");
    CHECK(mapped[64] == 0x5A && mapped[65] == 0x3C && mapped[66] == 0xC3 && mapped[67] == 0xA5,
          "32-bit fill pattern is not collapsed to one byte");
    CHECK(memcmp(mapped + 96, update_bytes, sizeof(update_bytes)) == 0, "vkCmdUpdateBuffer bytes");
    vkUnmapMemory(device, dst_mem);

    VkBuffer staging = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE, readback_mem = VK_NULL_HANDLE;
    VkImage image_a = VK_NULL_HANDLE, image_b = VK_NULL_HANDLE;
    VkDeviceMemory image_a_mem = VK_NULL_HANDLE, image_b_mem = VK_NULL_HANDLE;
    CHECK_VK(make_buffer(device, 3U * 6U * 4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         &staging, &staging_mem), "make staging with padded rows");
    CHECK_VK(make_buffer(device, 4U * 3U * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &readback, &readback_mem), "make readback");
    CHECK_VK(make_image(device, VK_FORMAT_B8G8R8A8_UNORM, 4, 3, &image_a, &image_a_mem), "make image A");
    CHECK_VK(make_image(device, VK_FORMAT_B8G8R8A8_UNORM, 4, 3, &image_b, &image_b_mem), "make image B");
    CHECK(vkMapMemory(device, image_a_mem, 0, 16, 0, (void **)&mapped) == VK_ERROR_MEMORY_MAP_FAILED,
          "image memory is not directly mappable in 0.3");
    CHECK_VK(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "map staging");
    memset(mapped, 0xEE, 3U * 6U * 4U);
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint8_t *px = mapped + y * 24U + x * 4U;
            px[0] = (uint8_t)(x * 10U + y);       /* B */
            px[1] = (uint8_t)(100U + x);          /* G */
            px[2] = (uint8_t)(200U + y);          /* R */
            px[3] = 255;                          /* A */
        }
    }
    vkUnmapMemory(device, staging_mem);
    CHECK_VK(vkResetFences(device, 1, &fence), "reset fence for image test");

    CHECK_VK(vkBeginCommandBuffer(cbs[1], &begin), "begin image test");
    VkImageMemoryBarrier barrier = image_barrier(image_a, VK_IMAGE_LAYOUT_UNDEFINED,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                                 VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy upload;
    memset(&upload, 0, sizeof(upload));
    upload.bufferRowLength = 6;
    upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload.imageSubresource.layerCount = 1;
    upload.imageExtent.width = 4;
    upload.imageExtent.height = 3;
    upload.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cbs[1], staging, image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    barrier = image_barrier(image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    barrier = image_barrier(image_b, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
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
    barrier = image_barrier(image_b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy download;
    memset(&download, 0, sizeof(download));
    download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    download.imageSubresource.layerCount = 1;
    download.imageExtent.width = 4;
    download.imageExtent.height = 3;
    download.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cbs[1], image_b, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &download);
    CHECK_VK(vkEndCommandBuffer(cbs[1]), "end image test");
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[1];
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence), "submit image test");
    CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait image test");
    CHECK_VK(vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "map readback");
    CHECK(mapped[0] == 0 && mapped[1] == 100 && mapped[2] == 200 && mapped[3] == 255,
          "padded upload pixel (0,0)");
    const uint32_t last = (2U * 4U + 3U) * 4U;
    CHECK(mapped[last + 0] == 32 && mapped[last + 1] == 103 && mapped[last + 2] == 202,
          "padded upload + image copy pixel (3,2)");
    bool saw_sentinel = false;
    for (uint32_t i = 0; i < 4U * 3U * 4U; ++i)
        saw_sentinel = saw_sentinel || mapped[i] == 0xEE;
    CHECK(!saw_sentinel, "row padding sentinel never reaches the image");
    CHECK(fnv1a32(mapped, 4U * 3U * 4U) == 0xEEF103F9U, "staged image checksum");
    vkUnmapMemory(device, readback_mem);

    CHECK_VK(vkResetCommandPool(device, pool, 0), "reset pool for clear test");
    CHECK_VK(vkResetFences(device, 1, &fence), "reset fence for clear test");
    CHECK_VK(vkBeginCommandBuffer(cbs[0], &begin), "begin clear test");
    barrier = image_barrier(image_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
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
    vkCmdClearColorImage(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &clear_range);
    barrier = image_barrier(image_a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    vkCmdCopyImageToBuffer(cbs[0], image_a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &download);
    CHECK_VK(vkEndCommandBuffer(cbs[0]), "end clear test");
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cbs[0];
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence), "submit clear test");
    CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait clear test");
    CHECK_VK(vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped), "map clear readback");
    CHECK(fnv1a32(mapped, 4U * 3U * 4U) == 0x1DE4992DU,
          "BGRA clear writes BGR A bytes [255,128,64,255]");
    vkUnmapMemory(device, readback_mem);

    VkImage unsupported = VK_NULL_HANDLE;
    VkImageCreateInfo bad_image;
    memset(&bad_image, 0, sizeof(bad_image));
    bad_image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    bad_image.imageType = VK_IMAGE_TYPE_2D;
    bad_image.format = VK_FORMAT_R8G8B8A8_SRGB;
    bad_image.extent.width = 1;
    bad_image.extent.height = 1;
    bad_image.extent.depth = 1;
    bad_image.mipLevels = 1;
    bad_image.arrayLayers = 1;
    bad_image.samples = VK_SAMPLE_COUNT_1_BIT;
    bad_image.tiling = VK_IMAGE_TILING_OPTIMAL;
    bad_image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bad_image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(device, &bad_image, NULL, &unsupported) == VK_ERROR_FORMAT_NOT_SUPPORTED,
          "SRGB image creation fails closed in 0.3");

    vkDestroyBuffer(device, unbound, NULL);
    vkDestroyImage(device, image_b, NULL);
    vkFreeMemory(device, image_b_mem, NULL);
    vkDestroyImage(device, image_a, NULL);
    vkFreeMemory(device, image_a_mem, NULL);
    vkDestroyBuffer(device, readback, NULL);
    vkFreeMemory(device, readback_mem, NULL);
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, staging_mem, NULL);
    vkDestroyBuffer(device, dst, NULL);
    vkFreeMemory(device, dst_mem, NULL);
    vkDestroyBuffer(device, src, NULL);
    vkFreeMemory(device, src_mem, NULL);
    vkDestroySemaphore(device, semaphore_b, NULL);
    vkDestroySemaphore(device, semaphore_a, NULL);
    vkDestroyFence(device, fence2, NULL);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    if (failures == 0)
        puts("DN VULKAN DRIVER TEST: PASS");
    else
        fprintf(stderr, "DN VULKAN DRIVER TEST: %d FAILURES\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
