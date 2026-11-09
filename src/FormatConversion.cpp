#include "FormatConversion.h"

#include <cstdio>
#include <optional>

namespace HdrLayer
{

static std::optional<uint32_t> findMemoryType(const vkroots::VkDeviceDispatch &dispatch, uint32_t typeBits)
{
    VkPhysicalDeviceMemoryProperties properties;
    dispatch.GetPhysicalDeviceMemoryProperties(dispatch.PhysicalDevice, &properties);

    std::optional<uint32_t> memoryType;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i))) {
            continue;
        }
        if (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            return i;
        }
        if (!memoryType) {
            memoryType = i;
        }
    }
    return memoryType;
}

static std::optional<uint32_t> findQueueFamily(const vkroots::VkDeviceDispatch &dispatch, VkQueue queue)
{
    for (const VkDeviceQueueCreateInfo &queueInfo : dispatch.DeviceQueueInfos) {
        for (uint32_t i = 0; i < queueInfo.queueCount; i++) {
            VkQueue deviceQueue = VK_NULL_HANDLE;
            if (queueInfo.flags) {
                const VkDeviceQueueInfo2 info{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                    .flags = queueInfo.flags,
                    .queueFamilyIndex = queueInfo.queueFamilyIndex,
                    .queueIndex = i,
                };
                dispatch.GetDeviceQueue2(dispatch.Device, &info, &deviceQueue);
            } else {
                dispatch.GetDeviceQueue(dispatch.Device, queueInfo.queueFamilyIndex, i, &deviceQueue);
            }

            if (deviceQueue == queue) {
                return queueInfo.queueFamilyIndex;
            }
        }
    }
    return std::nullopt;
}

static bool supportsGraphics(const vkroots::VkDeviceDispatch &dispatch, uint32_t queueFamily)
{
    uint32_t count = 0;
    dispatch.GetPhysicalDeviceQueueFamilyProperties(dispatch.PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    dispatch.GetPhysicalDeviceQueueFamilyProperties(dispatch.PhysicalDevice, &count, families.data());
    return queueFamily < count && (families[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

static VkImageMemoryBarrier imageBarrier(VkImage image, uint32_t layers, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                         VkImageLayout oldLayout, VkImageLayout newLayout)
{
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers },
    };
}

VkFormat FormatConversion::getPresentableFormat(VkFormat format)
{
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : format;
}

bool FormatConversion::canConvert(const vkroots::VkDeviceDispatch &dispatch, const VkSwapchainCreateInfoKHR &createInfo)
{
    const VkFormat presentableFormat = getPresentableFormat(createInfo.imageFormat);
    if (presentableFormat == createInfo.imageFormat) {
        return false;
    }

    constexpr VkSwapchainCreateFlagsKHR unsupportedFlags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR | VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR
        | VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR;
    if ((createInfo.flags & unsupportedFlags) || vkroots::FindInChain<VkImageFormatListCreateInfo>(&createInfo)) {
        return false;
    }

    VkFormatProperties source;
    VkFormatProperties destination;
    dispatch.GetPhysicalDeviceFormatProperties(dispatch.PhysicalDevice, createInfo.imageFormat, &source);
    dispatch.GetPhysicalDeviceFormatProperties(dispatch.PhysicalDevice, presentableFormat, &destination);
    return (source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (destination.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

VkResult FormatConversion::create(const vkroots::VkDeviceDispatch &dispatch, PFN_vkSetDeviceLoaderData setDeviceLoaderData,
                                  const VkSwapchainCreateInfoKHR &createInfo, VkSwapchainKHR swapchain, std::unique_ptr<FormatConversion> &conversion)
{
    if (!setDeviceLoaderData) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t count = 0;
    VkResult result = dispatch.GetSwapchainImagesKHR(dispatch.Device, swapchain, &count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::vector<VkImage> swapchainImages(count);
    result = dispatch.GetSwapchainImagesKHR(dispatch.Device, swapchain, &count, swapchainImages.data());
    if (result != VK_SUCCESS) {
        return result;
    }

    std::unique_ptr<FormatConversion> newConversion{ new FormatConversion(dispatch, setDeviceLoaderData, createInfo) };
    for (VkImage swapchainImage : swapchainImages) {
        result = newConversion->createImage(createInfo, swapchainImage);
        if (result != VK_SUCCESS) {
            newConversion->destroy();
            return result;
        }
    }

    conversion = std::move(newConversion);
    return VK_SUCCESS;
}

FormatConversion::FormatConversion(const vkroots::VkDeviceDispatch &dispatch, PFN_vkSetDeviceLoaderData setDeviceLoaderData,
                                   const VkSwapchainCreateInfoKHR &createInfo)
    : m_dispatch(dispatch)
    , m_setDeviceLoaderData(setDeviceLoaderData)
    , m_extent(createInfo.imageExtent)
    , m_layers(createInfo.imageArrayLayers)
{
}

void FormatConversion::destroy()
{
    waitForBlits();
    destroyCommandBuffers();

    const VkDevice device = m_dispatch.Device;
    for (const Image &image : m_images) {
        m_dispatch.DestroyFence(device, image.blitFence, nullptr);
        m_dispatch.DestroySemaphore(device, image.blitDone, nullptr);
        m_dispatch.DestroyImage(device, image.image, nullptr);
        m_dispatch.FreeMemory(device, image.memory, nullptr);
    }
    m_images.clear();
}

VkResult FormatConversion::getImages(uint32_t *pCount, VkImage *pImages) const
{
    return vkroots::array(m_images, pCount, pImages, [](VkImage &out, const Image &image) { out = image.image; });
}

VkResult FormatConversion::blit(VkQueue queue, uint32_t imageIndex, uint32_t waitSemaphoreCount, const VkSemaphore *pWaitSemaphores,
                                VkSemaphore *pBlitDone)
{
    VkResult result = useQueue(queue);
    if (result != VK_SUCCESS) {
        return result;
    }

    const Image &image = m_images[imageIndex];
    const VkDevice device = m_dispatch.Device;
    result = m_dispatch.WaitForFences(device, 1, &image.blitFence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = recordBlit(image);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = m_dispatch.ResetFences(device, 1, &image.blitFence);
    if (result != VK_SUCCESS) {
        return result;
    }

    const std::vector<VkPipelineStageFlags> waitStages(waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = waitSemaphoreCount,
        .pWaitSemaphores = pWaitSemaphores,
        .pWaitDstStageMask = waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &image.commandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &image.blitDone,
    };
    result = m_dispatch.QueueSubmit(queue, 1, &submitInfo, image.blitFence);
    if (result != VK_SUCCESS) {
        return result;
    }

    *pBlitDone = image.blitDone;
    return VK_SUCCESS;
}

VkResult FormatConversion::createImage(const VkSwapchainCreateInfoKHR &createInfo, VkImage swapchainImage)
{
    const VkDevice device = m_dispatch.Device;
    Image &image = m_images.emplace_back(Image{ .swapchainImage = swapchainImage });

    const bool concurrent = createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT;
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = createInfo.imageFormat,
        .extent = { createInfo.imageExtent.width, createInfo.imageExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = createInfo.imageArrayLayers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = createInfo.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = createInfo.imageSharingMode,
        .queueFamilyIndexCount = concurrent ? createInfo.queueFamilyIndexCount : 0,
        .pQueueFamilyIndices = concurrent ? createInfo.pQueueFamilyIndices : nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = m_dispatch.CreateImage(device, &imageInfo, nullptr, &image.image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements requirements;
    m_dispatch.GetImageMemoryRequirements(device, image.image, &requirements);
    const std::optional<uint32_t> memoryType = findMemoryType(m_dispatch, requirements.memoryTypeBits);
    if (!memoryType) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    const VkMemoryAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = *memoryType,
    };
    result = m_dispatch.AllocateMemory(device, &allocateInfo, nullptr, &image.memory);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = m_dispatch.BindImageMemory(device, image.image, image.memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    result = m_dispatch.CreateSemaphore(device, &semaphoreInfo, nullptr, &image.blitDone);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkFenceCreateInfo fenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    return m_dispatch.CreateFence(device, &fenceInfo, nullptr, &image.blitFence);
}

VkResult FormatConversion::useQueue(VkQueue queue)
{
    if (queue == m_queue) {
        return VK_SUCCESS;
    }

    const std::optional<uint32_t> queueFamily = findQueueFamily(m_dispatch, queue);
    if (!queueFamily || !supportsGraphics(m_dispatch, *queueFamily)) {
        fprintf(stderr, "[HDR Layer] Can't convert the swapchain format, the present queue doesn't support graphics\n");
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    if (!m_commandPool || *queueFamily != m_queueFamily) {
        const VkResult result = createCommandBuffers(*queueFamily);
        if (result != VK_SUCCESS) {
            destroyCommandBuffers();
            return result;
        }
    }

    m_queue = queue;
    return VK_SUCCESS;
}

VkResult FormatConversion::createCommandBuffers(uint32_t queueFamily)
{
    VkResult result = waitForBlits();
    if (result != VK_SUCCESS) {
        return result;
    }
    destroyCommandBuffers();

    const VkDevice device = m_dispatch.Device;
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };
    result = m_dispatch.CreateCommandPool(device, &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        return result;
    }
    m_queueFamily = queueFamily;

    std::vector<VkCommandBuffer> commandBuffers(m_images.size());
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = uint32_t(commandBuffers.size()),
    };
    result = m_dispatch.AllocateCommandBuffers(device, &allocateInfo, commandBuffers.data());
    if (result != VK_SUCCESS) {
        return result;
    }

    for (size_t i = 0; i < m_images.size(); i++) {
        result = m_setDeviceLoaderData(device, commandBuffers[i]);
        if (result != VK_SUCCESS) {
            return result;
        }
        m_images[i].commandBuffer = commandBuffers[i];
    }
    return VK_SUCCESS;
}

void FormatConversion::destroyCommandBuffers()
{
    m_dispatch.DestroyCommandPool(m_dispatch.Device, m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    for (Image &image : m_images) {
        image.commandBuffer = VK_NULL_HANDLE;
    }
}

VkResult FormatConversion::waitForBlits() const
{
    std::vector<VkFence> fences;
    for (const Image &image : m_images) {
        if (image.blitFence) {
            fences.push_back(image.blitFence);
        }
    }
    if (fences.empty()) {
        return VK_SUCCESS;
    }
    return m_dispatch.WaitForFences(m_dispatch.Device, uint32_t(fences.size()), fences.data(), VK_TRUE, UINT64_MAX);
}

VkResult FormatConversion::recordBlit(const Image &image) const
{
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const VkResult result = m_dispatch.BeginCommandBuffer(image.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkImageMemoryBarrier toTransfer[] = {
        imageBarrier(image.image, m_layers, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        imageBarrier(image.swapchainImage, m_layers, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    m_dispatch.CmdPipelineBarrier(image.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                    0, nullptr, 0, nullptr, uint32_t(std::size(toTransfer)), toTransfer);

    const VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m_layers };
    const VkOffset3D extent{ int32_t(m_extent.width), int32_t(m_extent.height), 1 };
    const VkImageBlit region{
        .srcSubresource = layers,
        .srcOffsets = { {}, extent },
        .dstSubresource = layers,
        .dstOffsets = { {}, extent },
    };
    m_dispatch.CmdBlitImage(image.commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.swapchainImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    const VkImageMemoryBarrier toPresent[] = {
        imageBarrier(image.image, m_layers, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        imageBarrier(image.swapchainImage, m_layers, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
    };
    m_dispatch.CmdPipelineBarrier(image.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                    0, nullptr, 0, nullptr, uint32_t(std::size(toPresent)), toPresent);

    return m_dispatch.EndCommandBuffer(image.commandBuffer);
}

}
