#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint32_t> loadSpvFp16(
        JNIEnv* env, jobject assetsObject, const char* path, std::string& error) {
    AAssetManager* manager = AAssetManager_fromJava(env, assetsObject);
    if (!manager) {
        error = "AssetManager unavailable";
        return {};
    }
    AAsset* asset = AAssetManager_open(manager, path, AASSET_MODE_BUFFER);
    if (!asset) {
        error = std::string("Missing shader: ") + path;
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

static uint32_t hostVisibleType(
        VkPhysicalDevice pd,
        uint32_t bits,
        VkMemoryPropertyFlags* selectedFlags) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            if (selectedFlags) *selectedFlags = mp.memoryTypes[i].propertyFlags;
            return i;
        }
    }
    return UINT32_MAX;
}

static bool hasExtension(
        VkPhysicalDevice pd,
        const char* extensionName) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, extensions.data());
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, extensionName) == 0) return true;
    }
    return false;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_runFp16Test(
        JNIEnv* env,
        jclass,
        jobject assetsObject) {

    std::string error;
    std::vector<uint32_t> shaderCode =
            loadSpvFp16(env, assetsObject, "spv/fp16_affine.spv", error);
    if (shaderCode.empty()) {
        std::string msg = "FP16 shader load failed: " + error;
        return env->NewStringUTF(msg.c_str());
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (device) vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    };

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSS5 Vulkan FP16 Test";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        return env->NewStringUTF(("vkCreateInstance failed: " +
                                  std::to_string((int)r)).c_str());
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (!pdCount) {
        cleanup();
        return env->NewStringUTF("No Vulkan GPU found.");
    }

    std::vector<VkPhysicalDevice> devices(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, devices.data());
    VkPhysicalDevice pd = devices.front();

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);

    const bool hasFp16Extension =
            hasExtension(pd, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    if (!hasFp16Extension && props.apiVersion < VK_API_VERSION_1_2) {
        cleanup();
        return env->NewStringUTF("VK_KHR_shader_float16_int8 is not available.");
    }

    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR fp16Features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &fp16Features;
    vkGetPhysicalDeviceFeatures2(pd, &features2);

    if (!fp16Features.shaderFloat16) {
        cleanup();
        return env->NewStringUTF(
                "The extension exists, but shaderFloat16 feature is disabled/unsupported.");
    }

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueCount, queueProps.data());

    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueCount; ++i) {
        if (queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily = i;
            break;
        }
    }
    if (queueFamily == UINT32_MAX) {
        cleanup();
        return env->NewStringUTF("No compute queue.");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* enabledExtensions[] = {
            VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME
    };

    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR enabledFp16{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR};
    enabledFp16.shaderFloat16 = VK_TRUE;
    enabledFp16.shaderInt8 = VK_FALSE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &enabledFp16;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (props.apiVersion < VK_API_VERSION_1_2) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = enabledExtensions;
    }

    r = vkCreateDevice(pd, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        std::string msg = "vkCreateDevice(FP16) failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    constexpr uint32_t kCount = 256;
    constexpr VkDeviceSize kBytes = kCount * sizeof(uint32_t);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kBytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    r = vkCreateBuffer(device, &bci, nullptr, &buffer);
    if (r != VK_SUCCESS) {
        std::string msg = "vkCreateBuffer failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);

    VkMemoryPropertyFlags memoryFlags = 0;
    uint32_t memType = hostVisibleType(pd, req.memoryTypeBits, &memoryFlags);
    if (memType == UINT32_MAX) {
        cleanup();
        return env->NewStringUTF("No HOST_VISIBLE buffer memory.");
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType;

    r = vkAllocateMemory(device, &mai, nullptr, &memory);
    if (r != VK_SUCCESS) {
        std::string msg = "vkAllocateMemory failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    vkBindBufferMemory(device, buffer, memory, 0);

    void* mapped = nullptr;
    r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = "Initial map failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    // low half = 1.0 (0x3c00), high half = 2.0 (0x4000)
    constexpr uint32_t kInputPacked = 0x40003c00u;
    auto* words = static_cast<uint32_t*>(mapped);
    for (uint32_t i = 0; i < kCount; ++i) words[i] = kInputPacked;

    const bool coherent =
            (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device, 1, &range);
    }
    vkUnmapMemory(device, memory);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = shaderCode.size() * sizeof(uint32_t);
    smci.pCode = shaderCode.data();

    r = vkCreateShaderModule(device, &smci, nullptr, &shader);
    if (r != VK_SUCCESS) {
        std::string msg = "FP16 shader module failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo slci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 1;
    slci.pBindings = &binding;

    r = vkCreateDescriptorSetLayout(device, &slci, nullptr, &setLayout);
    if (r != VK_SUCCESS) {
        std::string msg = "Descriptor layout failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    struct Push {
        uint32_t count;
        float scale;
        float bias;
    };

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(Push);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;

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
        std::string msg = "FP16 compute pipeline failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

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

    VkDescriptorSet set = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &dsai, &set);
    if (r != VK_SUCCESS) {
        std::string msg = "Descriptor allocation failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = buffer;
    dbi.offset = 0;
    dbi.range = kBytes;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkCommandPoolCreateInfo cpciPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpciPool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpciPool.queueFamilyIndex = queueFamily;

    r = vkCreateCommandPool(device, &cpciPool, nullptr, &commandPool);
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

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cbai, &cmd);
    if (r != VK_SUCCESS) {
        std::string msg = "Command allocation failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
            0, 1, &set, 0, nullptr);

    Push push{kCount, 2.0f, 1.0f};
    vkCmdPushConstants(
            cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(push), &push);

    vkCmdDispatch(cmd, (kCount + 63u) / 64u, 1, 1);

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

    vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            1, &barrier,
            0, nullptr,
            0, nullptr);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device, &fci, nullptr, &fence);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    r = vkQueueSubmit(queue, 1, &submit, fence);
    if (r == VK_SUCCESS) {
        r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    }
    if (r != VK_SUCCESS) {
        std::string msg = "FP16 dispatch/wait failed: " + std::to_string((int)r);
        cleanup();
        return env->NewStringUTF(msg.c_str());
    }

    r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        std::string msg = "Result map failed: " + std::to_string((int)r);
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

    // Expected: (1 * 2 + 1) = 3.0 => 0x4200
    //           (2 * 2 + 1) = 5.0 => 0x4500
    constexpr uint32_t kExpectedPacked = 0x45004200u;
    words = static_cast<uint32_t*>(mapped);

    uint32_t correct = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (words[i] == kExpectedPacked) ++correct;
    }
    const uint32_t sample = words[0];
    vkUnmapMemory(device, memory);

    std::ostringstream out;
    out << (correct == kCount ? "PASS" : "FAIL") << "\n";
    out << "GPU: " << props.deviceName << "\n";
    out << "shaderFloat16: YES\n";
    out << "Operation: FP16 pair * 2 + 1\n";
    out << "Input halves: 1.0 / 2.0\n";
    out << "Expected halves: 3.0 / 5.0\n";
    out << "Packed output: 0x" << std::hex << sample << std::dec << "\n";
    out << "Validation: " << correct << "/" << kCount << " packed FP16 pairs correct";

    cleanup();
    return env->NewStringUTF(out.str().c_str());
}
