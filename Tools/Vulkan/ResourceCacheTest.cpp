#include "Engine/Source/Gx/CGxDeviceVulkan/ResourceCache.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/ResidentBuffer.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Vk(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("GPU fixture VkResult " + std::to_string(result)); }
template<class F> void Reject(F&& operation) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Invalid cache input was accepted");
}
struct OwnedImage {
    VkDevice device;
    GxVulkan::Image value;
    OwnedImage(VkDevice device, GxVulkan::Image image) : device(device), value(image) {}
    ~OwnedImage() { GxVulkan::DestroyImage(device, value); }
    OwnedImage(const OwnedImage&) = delete;
    OwnedImage& operator=(const OwnedImage&) = delete;
    GxVulkan::Image Release() { auto result = value; value = {}; return result; }
};
struct Fixture {
    VkInstance instance{}; VkDevice device{}; VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{};
    GxVulkan::Buffer readback;
    std::vector<GxVulkan::Image> retired;
    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto& image : retired) GxVulkan::DestroyImage(device, image);
            GxVulkan::DestroyBuffer(device, readback);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void Init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        Vk(vkCreateInstance(&create, nullptr, &instance));
        uint32_t count = 0; Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count); Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical{}; uint32_t family = 0;
        for (auto candidate : devices) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
            for (uint32_t i = 0; i < count; ++i) if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical = candidate; family = i; break;
            }
            if (physical) break;
        }
        Check(physical != VK_NULL_HANDLE, "No Vulkan graphics device");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        Vk(vkCreateDevice(physical, &deviceInfo, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = family;
        Vk(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
        Vk(vkAllocateCommandBuffers(device, &allocate, &command));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; Vk(vkCreateFence(device, &fenceInfo, nullptr, &fence));
        readback = GxVulkan::CreateBuffer(device, memory, 16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    void Begin() {
        Vk(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Vk(vkBeginCommandBuffer(command, &begin));
    }
    void Submit() {
        Vk(vkEndCommandBuffer(command)); Vk(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        Vk(vkQueueSubmit(queue, 1, &submit, fence)); Vk(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
    }
    void ClearAndCopy(GxVulkan::Image& image, bool blue) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image = image.image; barrier.subresourceRange = {image.aspect, 0, 1, 0, 1};
        barrier.oldLayout = image.layout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkClearColorValue colour{}; colour.float32[blue ? 2 : 0] = 1; colour.float32[3] = 1;
        vkCmdClearColorImage(command, image.image, barrier.newLayout, &colour, 1, &barrier.subresourceRange);
        barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.layout = barrier.newLayout;
        VkBufferImageCopy copy{}; copy.imageSubresource = {image.aspect, 0, 0, 1}; copy.imageExtent = {16, 16, 1};
        vkCmdCopyImageToBuffer(command, image.image, image.layout, readback.buffer, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &host, 0, nullptr);
    }
    void Pixel(bool blue) {
        const std::array<uint8_t, 4> expected = blue ? std::array<uint8_t, 4>{0, 0, 255, 255} : std::array<uint8_t, 4>{255, 0, 0, 255};
        Check(std::memcmp(readback.mapped, expected.data(), 4) == 0, "Reused image texels were stale or corrupted");
    }
};
GxVulkan::ImageDescription Description() {
    return {16, 16, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
}
void TestImages(Fixture& f) {
    using namespace GxVulkan;
    ResourceCache cache(f.device, f.memory);
    auto description = Description();
    OwnedImage first(f.device, cache.AcquireImage(description));
    const auto handle = first.value.image;
    const auto view = first.value.view;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    Vk(vkCreateSampler(f.device, &sampler, nullptr, &first.value.sampler));
    const auto allocationBytes = first.value.allocationBytes;
    Check(allocationBytes > 0, "Missing actual Vulkan allocation accounting");
    auto bad = description; bad.width = 0;
    Reject([&] { cache.AcquireImage(bad); });
    Reject([&] { cache.Collect(VK_NULL_HANDLE, f.retired); });
    auto changes = std::array<ImageDescription, 7>{description, description, description, description, description, description, description};
    ++changes[0].width; ++changes[1].height; ++changes[2].levels; changes[3].layers = 6;
    changes[4].format = VK_FORMAT_B8G8R8A8_UNORM; changes[5].aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    changes[6].usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    for (const auto& changed : changes) Check(!first.value.Matches(changed), "Image compatibility key omitted a creation parameter");
    f.Begin(); f.ClearAndCopy(first.value, false);
    f.retired.push_back(first.Release());
    Check(!cache.Collect(f.fence, f.retired), "Unsignalled fence admitted an image into the cache");
    Check(f.retired.size() == 1 && cache.CachedBytes() == 0, "Unfinished retirement changed ownership");
    OwnedImage pending(f.device, cache.AcquireImage(description));
    Check(pending.value.image != handle, "Image was reused before its submission completed");
    f.Submit(); f.Pixel(false);
    Check(cache.Collect(f.fence, f.retired), "Completed image was not collected");
    Check(cache.CachedBytes() == allocationBytes && f.retired.empty(), "Image cache accounting is incorrect");
    OwnedImage different(f.device, cache.AcquireImage(changes[0]));
    Check(different.value.image != handle && cache.CachedBytes() == allocationBytes, "Incompatible image was reused");
    OwnedImage reused(f.device, cache.AcquireImage(description));
    Check(reused.value.image == handle && reused.value.view == view, "Matching image allocation/view was not reused");
    Check(reused.value.layout == VK_IMAGE_LAYOUT_UNDEFINED && !reused.value.sampler, "Reused image retained caller-specific state");
    Check(cache.CachedBytes() == 0 && cache.Stats().imageReuses == 1, "Image cache checkout accounting is incorrect");
    f.Begin(); f.ClearAndCopy(reused.value, true); f.Submit(); f.Pixel(true);
    {
        ResourceCache limited(f.device, f.memory, allocationBytes);
        OwnedImage a(f.device, limited.AcquireImage(description)), b(f.device, limited.AcquireImage(description));
        const auto newest = b.value.image;
        f.retired.push_back(a.Release()); f.retired.push_back(b.Release());
        Check(limited.Collect(f.fence, f.retired), "Budgeted retirement failed");
        Check(limited.CachedBytes() == allocationBytes && limited.Stats().evictions == 1, "Cache failed its byte budget/eviction policy");
        OwnedImage recent(f.device, limited.AcquireImage(description));
        Check(recent.value.image == newest, "Cache did not evict its oldest idle image");
    }
    for (VkDeviceSize budget : {VkDeviceSize(0), allocationBytes - 1}) {
        ResourceCache limited(f.device, f.memory, budget);
        OwnedImage image(f.device, limited.AcquireImage(description)); f.retired.push_back(image.Release());
        Check(limited.Collect(f.fence, f.retired) && limited.CachedBytes() == 0, "Oversized image escaped the cache budget");
    }
    {
        ResourceCache bounded(f.device, f.memory, allocationBytes * (ResourceCache::EntryLimit + 1));
        for (std::size_t i = 0; i <= ResourceCache::EntryLimit; ++i) {
            OwnedImage image(f.device, bounded.AcquireImage(description)); f.retired.push_back(image.Release());
        }
        Check(bounded.Collect(f.fence, f.retired), "Entry-limited retirement failed");
        Check(bounded.CachedBytes() == allocationBytes * ResourceCache::EntryLimit && bounded.Stats().evictions == 1,
            "Cache metadata entry limit was not enforced");
    }
    std::cout << "Image cache: exact compatibility, unsignalled fence, GPU texels, budget and eviction passed\n";
}
void TestImageChurn(Fixture& f) {
    using namespace GxVulkan;
    for (VkDeviceSize budget : {VkDeviceSize(0), ResourceCache::DefaultBudget}) {
        ResourceCache cache(f.device, f.memory, budget);
        for (unsigned i = 0; i < 32; ++i) {
            OwnedImage image(f.device, cache.AcquireImage(Description()));
            f.Begin(); f.ClearAndCopy(image.value, i % 2 != 0); f.retired.push_back(image.Release());
            f.Submit(); f.Pixel(i % 2 != 0); Check(cache.Collect(f.fence, f.retired), "Image churn retirement failed");
            Check(cache.CachedBytes() <= budget, "Image churn exceeded the byte budget");
        }
        Check(cache.Stats().imageAllocations == (budget ? 1 : 32), "Image cache did not reduce allocations deterministically");
        Check(cache.Stats().imageReuses == (budget ? 31 : 0), "Image cache reuse count mismatch");
        std::cout << "Image churn: allocations=" << cache.Stats().imageAllocations << " reuses=" << cache.Stats().imageReuses << '\n';
    }
}
}
int main() {
    try {
        Fixture fixture; fixture.Init(); TestImages(fixture); TestImageChurn(fixture);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
