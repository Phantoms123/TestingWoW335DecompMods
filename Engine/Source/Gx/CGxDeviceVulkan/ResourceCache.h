#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESOURCECACHE_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESOURCECACHE_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace GxVulkan {
struct ImageDescription {
    uint32_t width, height, levels, layers;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkDeviceSize allocationBytes = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool Matches(const ImageDescription& description) const;
};
void DestroyImage(VkDevice device, Image& image);

struct ResourceCacheStats {
    uint64_t imageAllocations = 0, imageReuses = 0, evictions = 0;
};
// Owns ONLY idle allocations. Retired resources stay with the caller until
// Collect verifies their submission fence. The budget counts Vulkan allocation
// requirements, not texel estimates; live/in-flight storage is not cache storage.
class ResourceCache {
public:
    static constexpr VkDeviceSize DefaultBudget = 32 * 1024 * 1024;
    static constexpr std::size_t EntryLimit = 128;
    ResourceCache(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
        VkDeviceSize budget = DefaultBudget);
    ~ResourceCache();
    ResourceCache(const ResourceCache&) = delete;
    ResourceCache& operator=(const ResourceCache&) = delete;
    Image AcquireImage(const ImageDescription& description);
    bool Collect(VkFence completion, std::vector<Image>& retired);
    VkDeviceSize CachedBytes() const { return m_cachedBytes; }
    const ResourceCacheStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }
private:
    void Recycle(Image& image);
    void EvictImage();
    VkDevice m_device;
    VkPhysicalDeviceMemoryProperties m_memory;
    VkDeviceSize m_budget, m_cachedBytes = 0;
    std::vector<Image> m_images;
    ResourceCacheStats m_stats;
};
}
#endif
