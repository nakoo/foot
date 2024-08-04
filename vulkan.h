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

struct vk_buffer {
    int fd;
    uint32_t width, height, stride;
    void *data;

    struct wl_buffer *wl_buf;
    pixman_image_t **pix;
    size_t pix_instances;

    unsigned age;

    /*
     * First item in the array is used to track frame-to-frame
     * damage. This is used when re-applying damage from the last
     * frame, when the compositor doesn't release buffers immediately
     * (forcing us to double buffer)
     *
     * The remaining items are used to track surface damage. Each
     * worker thread adds its own cell damage to "its" region. When
     * the frame is done, all damage is converted to a single region,
     * which is then used in calls to wl_surface_damage_buffer().
     */
    pixman_region32_t *dirty;
};

struct vk_buffer_chain;

struct vk_buffer_chain *vk_chain_new(struct vulkan *vk, struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1, bool scrollable, size_t pix_instances);
void vk_chain_free(struct vk_buffer_chain *chain);

struct vk_buffer *vk_get_buffer(struct vk_buffer_chain *chain, int wdth, int height, bool with_alpha);

void vk_get_many(
    struct vk_buffer_chain *chain, size_t count,
    int widths[static count], int heights[static count],
    struct vk_buffer *bufs[static count], bool with_alpha);

void vk_did_not_use_buf(struct vk_buffer *buf);

bool vk_can_scroll(const struct vk_buffer *buf);
bool vk_scroll(struct vk_buffer *buf, int rows,
                int top_margin, int top_keep_rows,
                int bottom_margin, int bottom_keep_rows);

void vk_addref(struct vk_buffer *buf);
void vk_unref(struct vk_buffer *buf);

void vk_purge(struct vk_buffer_chain *chain);
