#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint32_t> loadSpvUpscale(
        JNIEnv* env, jobject assetsObject, const char* path, std::string& error) {
    AAssetManager* manager = AAssetManager_fromJava(env, assetsObject);
    if (!manager) {
        error = "AssetManager unavailable";
        return {};
    }

    AAsset* asset = AAssetManager_open(manager, path, AASSET_MODE_BUFFER);
    if (!asset) {
        error = std::string("Missing shader asset: ") + path;
        return {};
    }

    const off_t length = AAsset_getLength(asset);
    if (length <= 0 || (length % 4) != 0) {
        AAsset_close(asset);
        error = "Invalid SPIR-V size";
        return {};
    }

    std::vector<uint32_t> words(static_cast<size_t>(length / 4));
    const int64_t got = AAsset_read(asset, words.data(), static_cast<size_t>(length));
    AAsset_close(asset);

    if (got != length) {
        error = "Incomplete SPIR-V read";
        return {};
    }
    return words;
}

static uint32_t findHostVisibleMemory(
        VkPhysicalDevice physical,
        uint32_t bits,
        VkMemoryPropertyFlags* flagsOut) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            if (flagsOut) *flagsOut = props.memoryTypes[i].propertyFlags;
            return i;
        }
    }
    return UINT32_MAX;
}

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkMemoryPropertyFlags flags = 0;
    VkDeviceSize size = 0;
};

static bool createHostBuffer(
        VkPhysicalDevice physical,
        VkDevice device,
        VkDeviceSize size,
        HostBuffer& out,
        std::string& error) {
    out.size = size;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(device, &bi, nullptr, &out.buffer);
    if (r != VK_SUCCESS) {
        error = "vkCreateBuffer: " + std::to_string((int)r);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);

    uint32_t memoryType =
            findHostVisibleMemory(physical, req.memoryTypeBits, &out.flags);
    if (memoryType == UINT32_MAX) {
        error = "No HOST_VISIBLE memory for storage buffer";
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType;

    r = vkAllocateMemory(device, &ai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        error = "vkAllocateMemory: " + std::to_string((int)r);
        return false;
    }

    r = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (r != VK_SUCCESS) {
        error = "vkBindBufferMemory: " + std::to_string((int)r);
        return false;
    }
    return true;
}

static void destroyHostBuffer(VkDevice device, HostBuffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = {};
}

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255u) {
    return (r & 255u) |
           ((g & 255u) << 8u) |
           ((b & 255u) << 16u) |
           ((a & 255u) << 24u);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_runUpscaleTest(
        JNIEnv* env,
        jclass,
        jobject assetManagerObject) {

    std::string error;
    auto shaderCode = loadSpvUpscale(
            env, assetManagerObject, "spv/upscale_bilinear.spv", error);
    if (shaderCode.empty()) {
        std::string msg = "Upscale shader load failed: " + error;
        return env->NewStringUTF(msg.c_str());
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    HostBuffer srcBuffer{};
    HostBuffer dstBuffer{};

    auto cleanup = [&]() {
        if (device) vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (device) {
            destroyHostBuffer(device, srcBuffer);
            destroyHostBuffer(device, dstBuffer);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    };

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Android Vulkan Upscaler";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        std::string msg = "vkCreateInstance failed: " + std::to_string((int)r);
        return env->NewStringUTF(msg.c_str());
    }

    uint32_t physicalCount = 0;
    vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
    if (physicalCount == 0) {
        cleanup();
        return env->NewStringUTF("No Vulkan GPU found.");
    }

    std::vector<VkPhysicalDevice> physicals(physicalCount);
    vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data());
    VkPhysicalDevice physical = physicals.front();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
            physical, &queueCount, queueProps.data());

    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueCount; ++i) {
        if (queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) {
        cleanup();
        return env->NewStringUTF("No Vulkan compute queue.");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    r = vkCreateDevice(physical, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        std::string msg = "vkCreateDevice failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    constexpr uint32_t srcW = 32;
    constexpr uint32_t srcH = 32;
    constexpr uint32_t dstW = 64;
    constexpr uint32_t dstH = 64;

    const VkDeviceSize srcBytes = srcW * srcH * sizeof(uint32_t);
    const VkDeviceSize dstBytes = dstW * dstH * sizeof(uint32_t);

    if (!createHostBuffer(physical, device, srcBytes, srcBuffer, error) ||
        !createHostBuffer(physical, device, dstBytes, dstBuffer, error)) {
        std::string msg = "Buffer setup failed: " + error;
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    void* mapped = nullptr;
    r = vkMapMemory(device, srcBuffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = "Source map failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    auto* src = static_cast<uint32_t*>(mapped);
    for (uint32_t y = 0; y < srcH; ++y) {
        for (uint32_t x = 0; x < srcW; ++x) {
            const uint32_t red = x * 255u / (srcW - 1u);
            const uint32_t green = y * 255u / (srcH - 1u);
            const uint32_t blue = ((x / 4u + y / 4u) & 1u) ? 220u : 48u;
            src[y * srcW + x] = rgba(red, green, blue);
        }
    }

    const bool srcCoherent =
            (srcBuffer.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!srcCoherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = srcBuffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device, 1, &range);
    }
    vkUnmapMemory(device, srcBuffer.memory);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = shaderCode.size() * sizeof(uint32_t);
    smci.pCode = shaderCode.data();

    r = vkCreateShaderModule(device, &smci, nullptr, &shader);
    if (r != VK_SUCCESS) {
        std::string msg = "vkCreateShaderModule failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo slci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 2;
    slci.pBindings = bindings;

    r = vkCreateDescriptorSetLayout(device, &slci, nullptr, &setLayout);
    if (r != VK_SUCCESS) {
        std::string msg = "Descriptor layout failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(uint32_t) * 4;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;

    r = vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout);
    if (r != VK_SUCCESS) {
        std::string msg = "Pipeline layout failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = pipelineLayout;

    r = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        std::string msg = "Compute pipeline failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;

    r = vkCreateDescriptorPool(device, &dpci, nullptr, &descriptorPool);
    if (r != VK_SUCCESS) {
        std::string msg = "Descriptor pool failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorSetAllocateInfo dsai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &dsai, &descriptorSet);
    if (r != VK_SUCCESS) {
        std::string msg = "Descriptor set alloc failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorBufferInfo infos[2]{};
    infos[0].buffer = srcBuffer.buffer;
    infos[0].offset = 0;
    infos[0].range = srcBytes;
    infos[1].buffer = dstBuffer.buffer;
    infos[1].offset = 0;
    infos[1].range = dstBytes;

    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    commandPoolInfo.queueFamilyIndex = queueFamily;

    r = vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool);
    if (r != VK_SUCCESS) {
        std::string msg = "Command pool failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkCommandBufferAllocateInfo cbai{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cbai, &commandBuffer);
    if (r != VK_SUCCESS) {
        std::string msg = "Command buffer alloc failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    r = vkBeginCommandBuffer(commandBuffer, &begin);
    if (r != VK_SUCCESS) {
        std::string msg = "Command begin failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout,
            0, 1, &descriptorSet,
            0, nullptr);

    struct Params {
        uint32_t srcWidth;
        uint32_t srcHeight;
        uint32_t dstWidth;
        uint32_t dstHeight;
    } params{srcW, srcH, dstW, dstH};

    vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(params),
            &params);

    vkCmdDispatch(commandBuffer, (dstW + 7u) / 8u, (dstH + 7u) / 8u, 1);

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

    vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            1, &barrier,
            0, nullptr,
            0, nullptr);

    r = vkEndCommandBuffer(commandBuffer);
    if (r != VK_SUCCESS) {
        std::string msg = "Command end failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = vkCreateFence(device, &fci, nullptr, &fence);
    if (r != VK_SUCCESS) {
        std::string msg = "Fence failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;

    r = vkQueueSubmit(queue, 1, &submit, fence);
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    }
    if (r != VK_SUCCESS) {
        std::string msg = "Upscale dispatch failed/wait timed out: " +
                          std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    r = vkMapMemory(device, dstBuffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = "Output map failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    const bool dstCoherent =
            (dstBuffer.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!dstCoherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = dstBuffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    }

    const auto* dst = static_cast<const uint32_t*>(mapped);
    uint64_t checksum = 1469598103934665603ULL;
    for (uint32_t i = 0; i < dstW * dstH; ++i) {
        checksum ^= dst[i];
        checksum *= 1099511628211ULL;
    }

    const uint32_t first = dst[0];
    const uint32_t center = dst[(dstH / 2u) * dstW + (dstW / 2u)];
    const uint32_t last = dst[dstW * dstH - 1u];
    const bool valid = first != 0u && center != 0u && last != 0u && checksum != 0u;

    vkUnmapMemory(device, dstBuffer.memory);

    std::ostringstream out;
    out << (valid ? "PASS" : "FAIL") << "\n";
    out << "GPU: " << properties.deviceName << "\n";
    out << "Input: " << srcW << "x" << srcH << " RGBA8\n";
    out << "Output: " << dstW << "x" << dstH << " RGBA8\n";
    out << "Scale: 2.0x\n";
    out << "Algorithm: bilinear Vulkan compute\n";
    out << "Dispatch: 8x8 workgroups\n";
    out << "Checksum: " << checksum << "\n";
    out << "Samples: " << first << " / " << center << " / " << last;

    cleanup();
    return env->NewStringUTF(out.str().c_str());
}
