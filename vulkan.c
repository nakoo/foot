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
#define LOG_ENABLE_DBG 0
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

static int
vulkan_find_mem_type(struct vulkan *vk, VkMemoryPropertyFlags flags, uint32_t req_bits)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &props);

    for (unsigned i = 0u; i < props.memoryTypeCount; ++i)
        if (req_bits & (1 << i))
            if ((props.memoryTypes[i].propertyFlags & flags) == flags)
                return i;

    return -1;
}

static tll(struct vk_buffer_private *) vk_deferred;

struct vk_buffer_chain;
struct vk_buffer_private {
    struct vk_buffer public;
    struct vk_buffer_chain *chain;

    size_t ref_count;
    bool busy;

    bool with_alpha;

    struct vulkan *vk;
    VkBuffer buffer;
    VkDeviceMemory memory;
};

static void
vk_buffer_destroy_dont_close(struct vk_buffer *buf)
{
    if (buf->pix != NULL)
        for (size_t i = 0; i < buf->pix_instances; i++)
            if (buf->pix[i] != NULL)
                pixman_image_unref(buf->pix[i]);

    free(buf->pix);
    buf->pix = NULL;
}

static void
vk_buffer_destroy(struct vk_buffer_private *buf)
{
    vk_buffer_destroy_dont_close(&buf->public);

    for (size_t i = 0; i < buf->public.pix_instances; i++)
        pixman_region32_fini(&buf->public.dirty[i]);
    free(buf->public.dirty);
    free(buf);
}

static bool
vk_buffer_unref_no_remove_from_chain(struct vk_buffer_private *buf)
{
    xassert(buf->ref_count > 0);
    buf->ref_count--;

    if (buf->ref_count > 0)
        return false;

    if (buf->busy)
        tll_push_back(vk_deferred, buf);
    else
        vk_buffer_destroy(buf);
    return true;
}

struct vk_buffer_chain {
    tll(struct vk_buffer_private *) bufs;
    struct vulkan *vk;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1;
    size_t pix_instances;
};

void
vk_purge(struct vk_buffer_chain *chain)
{
    LOG_DBG("chain: %p: purging all buffers", (void *)chain);

    /* Purge old buffers associated with this cookie */
    tll_foreach(chain->bufs, it) {
        if (vk_buffer_unref_no_remove_from_chain(it->item))
            tll_remove(chain->bufs, it);
    }
}

bool
vk_can_scroll(const struct vk_buffer *_buf)
{
    return false;
}

bool
vk_scroll(struct vk_buffer *_buf, int rows, int top_margin, int top_keep_rows, int bottom_margin, int bottom_keep_rows)
{
    return false;
}

void vk_addref(struct vk_buffer *_buf)
{
    struct vk_buffer_private *buf = (struct vk_buffer_private *)_buf;
    buf->ref_count++;
}

void
vk_unref(struct vk_buffer *_buf)
{
    if (_buf == NULL)
        return;

    struct vk_buffer_private *buf = (struct vk_buffer_private *)_buf;
    struct vk_buffer_chain *chain = buf->chain;

    tll_foreach(chain->bufs, it) {
        if (it->item != buf)
            continue;

        if (vk_buffer_unref_no_remove_from_chain(buf))
            tll_remove(chain->bufs, it);
        break;
    }
}

struct vk_buffer_chain *
vk_chain_new(struct vulkan *vk, struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1, bool _scrollable, size_t pix_instances)
{
    struct vk_buffer_chain *chain = xmalloc(sizeof(*chain));
    *chain = (struct vk_buffer_chain){
        .bufs = tll_init(),
        .vk = vk,
        .linux_dmabuf_v1 = linux_dmabuf_v1,
        .pix_instances = pix_instances,
    };
    return chain;
}

void
vk_chain_free(struct vk_buffer_chain *chain)
{
    if (chain == NULL)
        return;

    vk_purge(chain);

    if (tll_length(chain->bufs) > 0)
        BUG("chain=%p: there are buffers remaining; "
            "is there a missing call to vk_unref()?", (void *)chain);

    free(chain);
}

static void
vulkan_image_destroy(struct vk_buffer_private *img)
{
    if (img->public.pix) {
        for (size_t i = 0; i < img->public.pix_instances; i++)
            if (img->public.pix[i] != NULL)
                pixman_image_unref(img->public.pix[i]);
        free(img->public.pix);
    }
    if (img->public.data)
        vkUnmapMemory(img->vk->device, img->memory);
    if (img->public.fd != -1)
        close(img->public.fd);
    if (img->memory)
        vkFreeMemory(img->vk->device, img->memory, NULL);
    if (img->buffer)
        vkDestroyBuffer(img->vk->device, img->buffer, NULL);
    free(img);
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct vk_buffer_private *buffer = data;

    xassert(buffer->public.wl_buf == wl_buffer);
    xassert(buffer->busy);
    buffer->busy = false;

    if (buffer->ref_count == 0) {
        bool found = false;
        tll_foreach(vk_deferred, it) {
            if (it->item == buffer) {
                found = true;
                tll_remove(vk_deferred, it);
                break;
            }
        }

        vk_buffer_destroy(buffer);

        xassert(found);
        if (!found)
            LOG_WARN("deferred delete: buffer not on the 'deferred' list");
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

static struct vk_buffer *
vk_buffer_create(struct vk_buffer_chain *chain, int width, int height, bool with_alpha, bool immediate_purge)
{
    struct vulkan *vk = chain->vk;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1 = chain->linux_dmabuf_v1;

    struct vk_buffer_private *img = xmalloc(sizeof(*img));
    *img = (struct vk_buffer_private){
        .vk = vk,
        .public = (struct vk_buffer) {
            .fd = -1,
            .width = width,
            .height = height,
            .stride = width * 4,
        },
    };

    uint64_t mod = DRM_FORMAT_MOD_LINEAR;

    uint64_t mods[1] = { mod };
    VkImageDrmFormatModifierListCreateInfoEXT drm_format_mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = mods,
    };

    VkExternalMemoryBufferCreateInfo ext_mem = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .pNext = &drm_format_mod,
    };

    VkBufferCreateInfo buf_create = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext_mem,
        .size = width * height * 4,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(vk->device, &buf_create, NULL, &img->buffer) != VK_SUCCESS) {
        LOG_ERR("Could not allocate image");
        goto error;
    }

    VkMemoryRequirements mem_reqs = {0};
    vkGetBufferMemoryRequirements(vk->device, img->buffer, &mem_reqs);

    VkExportMemoryAllocateInfo export_mem = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    // The flags we need:
    // - HOST_VISIBLE in order to CPU map the buffer
    // - HOST_COHERENT as we otherwise need to call vkFlushMappedMemoryRanges after a write
    // - HOST_CACHED in order to have decent CPU access performancea
    int mem_type_index = vulkan_find_mem_type(vk,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            mem_reqs.memoryTypeBits);

    if (mem_type_index == -1) {
        LOG_ERR("Could not find suitable memory type");
        goto error;
    }

    VkMemoryAllocateInfo mem_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_mem,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_index,
    };

    if (vkAllocateMemory(vk->device, &mem_alloc, NULL, &img->memory) != VK_SUCCESS) {
        LOG_ERR("Could not allocate memory");
        goto error;
    }

    if (vkBindBufferMemory(vk->device, img->buffer, img->memory, 0) != VK_SUCCESS) {
        LOG_ERR("Could not bind memory");
        goto error;
    }

    VkMemoryGetFdInfoKHR mem_get_fd = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = img->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    if (vk->api.vkGetMemoryFdKHR(vk->device, &mem_get_fd, &img->public.fd) != VK_SUCCESS) {
        LOG_ERR("Could not get dmabuf");
        goto error;
    }

    if (vkMapMemory(vk->device, img->memory, 0, VK_WHOLE_SIZE, 0, &img->public.data) != VK_SUCCESS) {
        LOG_ERR("Could not map memory");
        goto error;
    }

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(linux_dmabuf_v1);
    zwp_linux_buffer_params_v1_add(params, img->public.fd, 0, 0, width * 4, mod >> 32, mod & 0xFF);
    img->public.wl_buf = zwp_linux_buffer_params_v1_create_immed(params, width, height,
            with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888, 0);

    wl_buffer_add_listener(img->public.wl_buf, &buffer_listener, img);

    img->public.pix_instances = chain->pix_instances;
    img->public.age = 1234;
    img->chain = chain;
    img->ref_count = immediate_purge ? 0 : 1;
    img->busy = true;
    img->with_alpha = with_alpha;


    img->public.pix = xcalloc(img->public.pix_instances, sizeof(*img->public.pix));

    /* One pixman image for each worker thread (do we really need multiple?) */
    for (size_t i = 0; i < img->public.pix_instances; i++) {
        img->public.pix[i] = pixman_image_create_bits_no_clear(
            img->with_alpha ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8,
            width, height, img->public.data, img->public.stride);
        if (img->public.pix[i] == NULL) {
            LOG_ERR("failed to create pixman image");
            goto error;
        }
    }

    if (immediate_purge)
        tll_push_front(vk_deferred, img);
    else
        tll_push_front(chain->bufs, img);

    img->public.dirty = xmalloc(chain->pix_instances * sizeof(img->public.dirty[0]));

    for (size_t j = 0; j < chain->pix_instances; j++)
        pixman_region32_init(&img->public.dirty[j]);

    return &img->public;

error:
    vulkan_image_destroy(img);
    return NULL;
}

struct vk_buffer *
vk_get_buffer(struct vk_buffer_chain *chain, int width, int height, bool with_alpha)
{
    LOG_DBG(
        "chain=%p: looking for a reusable %dx%d buffer "
        "among %zu potential buffers",
        (void *)chain, width, height, tll_length(chain->bufs));

    struct vk_buffer_private *cached = NULL;
    tll_foreach(chain->bufs, it) {
        struct vk_buffer_private *buf = it->item;

        if (buf->public.width != width || buf->public.height != height ||
               with_alpha != buf->with_alpha) {
            LOG_DBG("purging mismatching buffer %p", (void *)buf);
            if (vk_buffer_unref_no_remove_from_chain(buf))
                tll_remove(chain->bufs, it);
            continue;
        }

        if (buf->busy) {
            buf->public.age++;
            continue;
        }

        if (cached == NULL) {
            cached = buf;
        } else {
            /* We have multiple buffers eligible for
             * reuse. Pick the "youngest" one, and mark the
             * other one for purging */
            if (buf->public.age < cached->public.age) {
                vk_unref(&cached->public);
                cached = buf;
            } else {
                if (vk_buffer_unref_no_remove_from_chain(buf))
                    tll_remove(chain->bufs, it);
            }
        }
    }

    if (cached != NULL) {
        LOG_DBG("re-using buffer %p from cache", (void *)cached);
        cached->busy = true;
        for (size_t i = 0; i < cached->public.pix_instances; i++)
            pixman_region32_clear(&cached->public.dirty[i]);
        xassert(cached->public.pix_instances == chain->pix_instances);
        return &cached->public;
    }

    return vk_buffer_create(chain, width, height, with_alpha, false);
}

void
vk_did_not_use_buf(struct vk_buffer *_buf)
{
    struct vk_buffer_private *buf = (struct vk_buffer_private *)_buf;
    buf->busy = false;
}

void
vk_get_many(
    struct vk_buffer_chain *chain, size_t count,
    int widths[static count], int heights[static count],
    struct vk_buffer *bufs[static count], bool with_alpha)
{
    for (size_t idx = 0; idx < count; idx++)
        bufs[count] = vk_buffer_create(chain, widths[idx], heights[idx], with_alpha, true);
}