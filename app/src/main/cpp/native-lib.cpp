#include <jni.h>
#include <vulkan/vulkan.h>
#include <sstream>
#include <vector>
#include <cstring>

static const char* deviceType(VkPhysicalDeviceType t) {
    switch(t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "Other";
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_probe(JNIEnv* env, jclass) {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "DLSS5 Vulkan Lab";
    ai.applicationVersion = VK_MAKE_VERSION(0,2,0);
    ai.pEngineName = "Mobile Vulkan Compute";
    ai.engineVersion = VK_MAKE_VERSION(0,2,0);
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &instance);
    if (r != VK_SUCCESS) {
        std::string s = "vkCreateInstance failed: " + std::to_string((int)r);
        return env->NewStringUTF(s.c_str());
    }

    uint32_t count=0;
    vkEnumeratePhysicalDevices(instance,&count,nullptr);
    if (!count) {
        vkDestroyInstance(instance,nullptr);
        return env->NewStringUTF("Vulkan initialized, but no physical GPU was exposed.");
    }
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance,&count,devs.data());
    VkPhysicalDeviceProperties p{};
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceProperties(devs[0],&p);
    vkGetPhysicalDeviceFeatures(devs[0],&f);

    uint32_t qcount=0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[0],&qcount,nullptr);
    std::vector<VkQueueFamilyProperties> qs(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(devs[0],&qcount,qs.data());
    bool compute=false;
    for (auto &q:qs) if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) compute=true;

    std::ostringstream out;
    out << "GPU: " << p.deviceName << "\n";
    out << "Type: " << deviceType(p.deviceType) << "\n";
    out << "Vulkan API: "
        << VK_VERSION_MAJOR(p.apiVersion) << "."
        << VK_VERSION_MINOR(p.apiVersion) << "."
        << VK_VERSION_PATCH(p.apiVersion) << "\n";
    out << "Compute queue: " << (compute?"YES":"NO") << "\n";
    out << "Shader float64: " << (f.shaderFloat64?"YES":"NO") << "\n";
    out << "Max compute workgroup invocations: " << p.limits.maxComputeWorkGroupInvocations << "\n";
    out << "Max shared memory: " << p.limits.maxComputeSharedMemorySize << " bytes";
    vkDestroyInstance(instance,nullptr);
    return env->NewStringUTF(out.str().c_str());
}
