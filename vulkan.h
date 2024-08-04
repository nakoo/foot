#pragma once

#include <sys/types.h>
#include <vulkan/vulkan.h>

struct vulkan {
    VkInstance instance;
    VkPhysicalDevice *devices;
    uint32_t device_len;
    VkPhysicalDevice physical_device;

    VkDevice device;

    struct {
        PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
    } api;
};

void vulkan_destroy(struct vulkan *vk);
struct vulkan *vulkan_create(dev_t preferred_device);