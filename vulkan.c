#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <stdbool.h>
#include <pixman.h>
#include <tllist.h>
#include "linux-dmabuf-v1.h"
#include "vulkan.h"

#define LOG_MODULE "vulkan"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "xmalloc.h"

static void
log_phdev(const VkPhysicalDeviceProperties *props)
{
#if LOG_ENABLE_DBG == 1
    uint32_t vv_major = VK_VERSION_MAJOR(props->apiVersion);
    uint32_t vv_minor = VK_VERSION_MINOR(props->apiVersion);
    uint32_t vv_patch = VK_VERSION_PATCH(props->apiVersion);

    uint32_t dv_major = VK_VERSION_MAJOR(props->driverVersion);
    uint32_t dv_minor = VK_VERSION_MINOR(props->driverVersion);
    uint32_t dv_patch = VK_VERSION_PATCH(props->driverVersion);

    const char *dev_type = "unknown";
    switch (props->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        dev_type = "integrated";
        break;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        dev_type = "discrete";
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        dev_type = "cpu";
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        dev_type = "vgpu";
        break;
    default:
        break;
    }

    LOG_DBG("Vulkan device: %s, type: %s, supported API version: %u.%u.%u, driver version: %u.%u.%u",
        props->deviceName, dev_type, vv_major, vv_minor, vv_patch, dv_major, dv_minor, dv_patch);
#endif
}

static int
vulkan_select_queue_family(struct vulkan *vk, VkPhysicalDevice phdev)
{
    uint32_t qfam_count;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &qfam_count, NULL);
    assert(qfam_count > 0);
    VkQueueFamilyProperties queue_props[qfam_count];
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &qfam_count, queue_props);

    for (unsigned i = 0u; i < qfam_count; ++i) {
        if (queue_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            return i;
        }
    }

    for (unsigned i = 0u; i < qfam_count; ++i) {
        if (queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }

    abort();
}

static bool
check_extension(const VkExtensionProperties *avail, uint32_t avail_len, const char *name)
{
    for (size_t i = 0; i < avail_len; i++)
        if (strcmp(avail[i].extensionName, name) == 0)
            return true;
    return false;
}

void
vulkan_destroy(struct vulkan *vk)
{
    if (vk->device)
        vkDestroyDevice(vk->device, NULL);
    if (vk->instance)
        vkDestroyInstance(vk->instance, NULL);
    free(vk);
}

struct vulkan *
vulkan_create(dev_t preferred_device)
{
    LOG_DBG("Creating vulkan backend");
    struct vulkan *vk = calloc(1, sizeof(*vk));
    if (!vk) {
        return NULL;
    }

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pEngineName = "foot",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };

    vk->instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, NULL, &vk->instance) != VK_SUCCESS) {
        LOG_ERR("Could not create Vulkan instance");
        goto error;
    }
    LOG_DBG("Created instance");

    //
    // Enumerate and pick a physical device. The enumeration can take
    // a little while at least on my machines
    //
    vkEnumeratePhysicalDevices(vk->instance, &vk->device_len, NULL);
    if (vk->device_len == 0) {
        LOG_ERR("No physical Vulkan devices");
        goto error;
    }

    vk->devices = calloc(vk->device_len+1, sizeof(*vk->devices));

    vkEnumeratePhysicalDevices(vk->instance, &vk->device_len, vk->devices);
    LOG_DBG("Enumerated physical Vulkan devices");

    int chosen = 0;
    for (uint32_t idx = 0; idx < vk->device_len; idx++) {
        VkPhysicalDevice phdev = vk->devices[idx];

        uint32_t avail_extc = 0;
        if (vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, NULL) != VK_SUCCESS || avail_extc == 0) {
            LOG_ERR("Could not enumerate device extensions");
            continue;
        }

        VkExtensionProperties avail_ext_props[avail_extc + 1];
        if (vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, avail_ext_props) != VK_SUCCESS) {
            LOG_ERR("Could not enumerate device extensions");
            continue;
        }

        bool has_drm_props = check_extension(avail_ext_props, avail_extc,
            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        if (!has_drm_props) {
            LOG_ERR("Device does not support DRM extension");
            continue;
        }

        VkPhysicalDeviceDrmPropertiesEXT drm_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
        };
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &drm_props,
        };
        vkGetPhysicalDeviceProperties2(phdev, &props);

        log_phdev(&props.properties);

        if (preferred_device == 0) {
            // Integrated GPUs are usually better for memory mapping
            if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                LOG_DBG("Selected integrated GPU");
                chosen = idx;
            }
            continue;
        }

        dev_t primary_devid = makedev(drm_props.primaryMajor, drm_props.primaryMinor);
        dev_t render_devid = makedev(drm_props.renderMajor, drm_props.renderMinor);
        if (primary_devid == preferred_device || render_devid == preferred_device) {
                LOG_DBG("Selected preferred physical Vulkan device");
                chosen = idx;
                break;
        }
    }

    vk->physical_device = vk->devices[chosen];
    LOG_DBG("Selected physical Vulkan device");

    const char *extensions[4] = { 0 };
    size_t extensions_len = 0;
    extensions[extensions_len++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    extensions[extensions_len++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    extensions[extensions_len++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;

    const float prio = 1.f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vulkan_select_queue_family(vk, vk->physical_device),
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };

    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1u,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = extensions_len,
        .ppEnabledExtensionNames = extensions,
    };

    if (vkCreateDevice(vk->physical_device, &dev_info, NULL, &vk->device) != VK_SUCCESS) {
        LOG_ERR("Could not create device");
        goto error;
    }
    LOG_DBG("Created logical Vulkan device");

    vk->api.vkGetMemoryFdKHR =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdKHR");

    return vk;

error:
    vulkan_destroy(vk);
    return NULL;
}