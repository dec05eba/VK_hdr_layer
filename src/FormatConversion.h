#pragma once

#include "vkroots.h"

#include <memory>
#include <vector>

namespace HdrLayer
{

/* Mesa can only present 10-bit X11 windows as A2R10G10B10, so swapchains with other 10-bit formats render to separate images
   that are blitted to the swapchain images when presented */
class FormatConversion
{
public:
    static VkFormat getPresentableFormat(VkFormat format);
    static bool canConvert(const vkroots::VkDeviceDispatch *pDispatch, const VkSwapchainCreateInfoKHR &createInfo);
    static VkResult create(const vkroots::VkDeviceDispatch *pDispatch, PFN_vkSetDeviceLoaderData setDeviceLoaderData,
                           const VkSwapchainCreateInfoKHR &createInfo, VkSwapchainKHR swapchain, std::unique_ptr<FormatConversion> &conversion);
    void destroy();

    VkResult getImages(uint32_t *pCount, VkImage *pImages) const;
    VkResult blit(VkQueue queue, uint32_t imageIndex, uint32_t waitSemaphoreCount, const VkSemaphore *pWaitSemaphores, VkSemaphore *pBlitDone);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImage swapchainImage = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore blitDone = VK_NULL_HANDLE;
        VkFence blitFence = VK_NULL_HANDLE;
    };

    FormatConversion(const vkroots::VkDeviceDispatch *pDispatch, PFN_vkSetDeviceLoaderData setDeviceLoaderData, const VkSwapchainCreateInfoKHR &createInfo);

    VkResult createImage(const VkSwapchainCreateInfoKHR &createInfo, VkImage swapchainImage);
    VkResult useQueue(VkQueue queue);
    VkResult createCommandBuffers(uint32_t queueFamily);
    void destroyCommandBuffers();
    VkResult waitForBlits() const;
    VkResult recordBlit(const Image &image) const;

    const vkroots::VkDeviceDispatch *m_pDispatch;
    PFN_vkSetDeviceLoaderData m_setDeviceLoaderData;
    VkExtent2D m_extent;
    uint32_t m_layers;
    std::vector<Image> m_images;

    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
};

}
