#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

static uint32_t findMemoryType(
        VkPhysicalDevice physicalDevice,
        uint32_t typeBits,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags* selectedFlags = nullptr) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required) {
            if (selectedFlags) *selectedFlags = props.memoryTypes[i].propertyFlags;
            return i;
        }
    }
    return UINT32_MAX;
}

static std::vector<uint32_t> loadSpirv(
        JNIEnv* env,
        jobject assetManagerObject,
        const char* assetPath,
        std::string& error) {
    AAssetManager* manager = AAssetManager_fromJava(env, assetManagerObject);
    if (!manager) {
        error = "AAssetManager_fromJava failed";
        return {};
    }

    AAsset* asset = AAssetManager_open(manager, assetPath, AASSET_MODE_BUFFER);
    if (!asset) {
        error = std::string("Could not open asset: ") + assetPath;
        return {};
    }

    const off_t length = AAsset_getLength(asset);
    if (length <= 0 || (length % 4) != 0) {
        AAsset_close(asset);
        error = "Invalid SPIR-V asset length";
        return {};
    }

    std::vector<uint32_t> words(static_cast<size_t>(length / 4));
    const int64_t read = AAsset_read(asset, words.data(), static_cast<size_t>(length));
    AAsset_close(asset);

    if (read != length) {
        error = "Failed to read complete SPIR-V asset";
        return {};
    }
    return words;
}

static std::string vkVersionString(uint32_t v) {
    std::ostringstream o;
    o << VK_VERSION_MAJOR(v) << "."
      << VK_VERSION_MINOR(v) << "."
      << VK_VERSION_PATCH(v);
    return o.str();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_probe(JNIEnv* env, jclass) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSS5 Vulkan Lab";
    app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    app.pEngineName = "Android Vulkan Compute";
    app.engineVersion = VK_MAKE_VERSION(0, 3, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::string msg = "vkCreateInstance failed: " + std::to_string((int) result);
        return env->NewStringUTF(msg.c_str());
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF("No Vulkan physical device exposed by Android.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    VkPhysicalDevice physicalDevice = devices.front();

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());

    int computeQueue = -1;
    for (uint32_t i = 0; i < queueCount; ++i) {
        if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeQueue = static_cast<int>(i);
            break;
        }
    }

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());

    bool fp16Ext = false;
    bool subgroupSizeControl = false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, "VK_KHR_shader_float16_int8") == 0) fp16Ext = true;
        if (std::strcmp(e.extensionName, "VK_EXT_subgroup_size_control") == 0) subgroupSizeControl = true;
    }

    std::ostringstream out;
    out << "GPU: " << props.deviceName << "\n";
    out << "Vulkan API: " << vkVersionString(props.apiVersion) << "\n";
    out << "Compute queue: " << (computeQueue >= 0 ? "YES" : "NO") << "\n";
    out << "FP16 extension: " << (fp16Ext ? "YES" : "NO") << "\n";
    out << "Subgroup size control: " << (subgroupSizeControl ? "YES" : "NO") << "\n";
    out << "Max workgroup invocations: " << props.limits.maxComputeWorkGroupInvocations << "\n";
    out << "Shared memory/workgroup: " << props.limits.maxComputeSharedMemorySize << " bytes";

    vkDestroyInstance(instance, nullptr);
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_runComputeTest(
        JNIEnv* env,
        jclass,
        jobject assetManagerObject) {

    std::string shaderError;
    std::vector<uint32_t> shaderCode =
            loadSpirv(env, assetManagerObject, "spv/compute_test.spv", shaderError);
    if (shaderCode.empty()) {
        std::string msg = "SPIR-V load failed: " + shaderError;
        return env->NewStringUTF(msg.c_str());
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    auto fail = [&](const std::string& where, VkResult code) {
        std::ostringstream o;
        o << where << " failed: VkResult " << static_cast<int>(code);
        return o.str();
    };

    auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, shaderModule, nullptr);
        if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    };

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSS5 Vulkan Lab Compute Test";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;

    VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateInstance", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    uint32_t physicalCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
    if (r != VK_SUCCESS || physicalCount == 0) {
        std::string msg = physicalCount == 0 ? "No Vulkan GPU found" : fail("vkEnumeratePhysicalDevices", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = physicalDevices.front();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queueProps.data());

    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueCount; ++i) {
        if (queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) {
        cleanup();
        return env->NewStringUTF("No compute-capable Vulkan queue found.");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;

    r = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateDevice", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    constexpr uint32_t kElementCount = 256;
    constexpr VkDeviceSize kBufferBytes = sizeof(uint32_t) * kElementCount;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = kBufferBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    r = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateBuffer", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    VkMemoryPropertyFlags memoryFlags = 0;
    uint32_t memoryType = findMemoryType(
            physicalDevice,
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            &memoryFlags);
    if (memoryType == UINT32_MAX) {
        cleanup();
        return env->NewStringUTF("No HOST_VISIBLE Vulkan memory type found.");
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;

    r = vkAllocateMemory(device, &allocation, nullptr, &memory);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkAllocateMemory", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    r = vkBindBufferMemory(device, buffer, memory, 0);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkBindBufferMemory", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    void* mapped = nullptr;
    r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkMapMemory(initial)", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }
    auto* input = static_cast<uint32_t*>(mapped);
    for (uint32_t i = 0; i < kElementCount; ++i) input[i] = i;

    const bool coherent = (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device, 1, &range);
    }
    vkUnmapMemory(device, memory);

    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = shaderCode.size() * sizeof(uint32_t);
    shaderInfo.pCode = shaderCode.data();

    r = vkCreateShaderModule(device, &shaderInfo, nullptr, &shaderModule);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateShaderModule", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount = 1;
    descriptorLayoutInfo.pBindings = &binding;

    r = vkCreateDescriptorSetLayout(
            device, &descriptorLayoutInfo, nullptr, &descriptorLayout);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateDescriptorSetLayout", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(uint32_t) * 2;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    r = vkCreatePipelineLayout(
            device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreatePipelineLayout", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName = "main";

    VkComputePipelineCreateInfo computePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computePipelineInfo.stage = stage;
    computePipelineInfo.layout = pipelineLayout;

    r = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &computePipelineInfo, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateComputePipelines", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateDescriptorPool", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorSetAllocateInfo setAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocation.descriptorPool = descriptorPool;
    setAllocation.descriptorSetCount = 1;
    setAllocation.pSetLayouts = &descriptorLayout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &setAllocation, &descriptorSet);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkAllocateDescriptorSets", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorBufferInfo descriptorBuffer{};
    descriptorBuffer.buffer = buffer;
    descriptorBuffer.offset = 0;
    descriptorBuffer.range = kBufferBytes;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &descriptorBuffer;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = queueFamily;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    r = vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateCommandPool", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkCommandBufferAllocateInfo commandAllocation{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = commandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &commandAllocation, &commandBuffer);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkAllocateCommandBuffers", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(commandBuffer, &begin);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkBeginCommandBuffer", r);
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

    struct PushConstants {
        uint32_t count;
        uint32_t addValue;
    } constants{kElementCount, 7};

    vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(constants),
            &constants);

    vkCmdDispatch(commandBuffer, (kElementCount + 63u) / 64u, 1, 1);

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
        std::string msg = fail("vkEndCommandBuffer", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkCreateFence", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;

    r = vkQueueSubmit(queue, 1, &submit, fence);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkQueueSubmit", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkWaitForFences", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = fail("vkMapMemory(result)", r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    if (!coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    }

    auto* output = static_cast<uint32_t*>(mapped);
    bool valid = true;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        if (output[i] != i + 7u) {
            valid = false;
            break;
        }
    }

    const uint32_t sample0 = output[0];
    const uint32_t sample1 = output[1];
    const uint32_t sample255 = output[255];
    vkUnmapMemory(device, memory);

    std::ostringstream report;
    report << (valid ? "PASS" : "FAIL") << "\n";
    report << "GPU: " << properties.deviceName << "\n";
    report << "Shader: compute_test.spv\n";
    report << "Dispatch: 4 workgroups x 64 threads\n";
    report << "Expected operation: data[i] += 7\n";
    report << "Samples: 0->" << sample0
           << ", 1->" << sample1
           << ", 255->" << sample255 << "\n";
    report << "Validation: " << (valid ? "256/256 values correct" : "mismatch detected");

    cleanup();
    return env->NewStringUTF(report.str().c_str());
}
