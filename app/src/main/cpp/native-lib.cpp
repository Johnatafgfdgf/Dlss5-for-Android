#include <jni.h>
#include <vulkan/vulkan.h>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdint>

static uint32_t findMemory(VkPhysicalDevice pd,uint32_t bits,VkMemoryPropertyFlags flags){
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&flags)==flags) return i;
    return UINT32_MAX;
}
static const char* deviceType(VkPhysicalDeviceType t){
    return t==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?"Integrated GPU":
           t==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?"Discrete GPU":"Other";
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_dlss5_vulkanlab_NativeVulkan_probe(JNIEnv* env,jclass){
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName="DLSS5 Vulkan Lab"; ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&ai;
    VkInstance inst{}; VkResult r=vkCreateInstance(&ici,nullptr,&inst);
    if(r!=VK_SUCCESS) return env->NewStringUTF(("vkCreateInstance failed: "+std::to_string((int)r)).c_str());
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr);
    if(!n){vkDestroyInstance(inst,nullptr);return env->NewStringUTF("No Vulkan GPU");}
    std::vector<VkPhysicalDevice> ds(n); vkEnumeratePhysicalDevices(inst,&n,ds.data()); auto pd=ds[0];
    VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd,&p);
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qp.data());
    int qi=-1; for(uint32_t i=0;i<qn;i++) if(qp[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qi=(int)i;break;}
    std::ostringstream o; o<<"GPU: "<<p.deviceName<<"\nType: "<<deviceType(p.deviceType)<<"\nVulkan: "<<VK_VERSION_MAJOR(p.apiVersion)<<"."<<VK_VERSION_MINOR(p.apiVersion)<<"\nCompute queue: "<<(qi>=0?"YES":"NO");
    if(qi<0){vkDestroyInstance(inst,nullptr);return env->NewStringUTF(o.str().c_str());}

    float priority=1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=qi;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice dev{};r=vkCreateDevice(pd,&dci,nullptr,&dev);
    if(r!=VK_SUCCESS){o<<"\nDevice creation: FAIL "<<r;vkDestroyInstance(inst,nullptr);return env->NewStringUTF(o.str().c_str());}

    const VkDeviceSize bytes=256*sizeof(uint32_t);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=bytes;bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf{};r=vkCreateBuffer(dev,&bci,nullptr,&buf);
    VkDeviceMemory mem{};
    if(r==VK_SUCCESS){
        VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(dev,buf,&mr);
        uint32_t mt=findMemory(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(mt!=UINT32_MAX){
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.allocationSize=mr.size;mai.memoryTypeIndex=mt;
            r=vkAllocateMemory(dev,&mai,nullptr,&mem);
            if(r==VK_SUCCESS){vkBindBufferMemory(dev,buf,mem,0);void* ptr=nullptr;r=vkMapMemory(dev,mem,0,bytes,0,&ptr);
                if(r==VK_SUCCESS){auto* v=(uint32_t*)ptr;for(uint32_t i=0;i<256;i++)v[i]=i;vkUnmapMemory(dev,mem);o<<"\nStorage buffer: PASS (1 KB mapped)";}
            }
        }
    }
    if(mem)vkFreeMemory(dev,mem,nullptr);if(buf)vkDestroyBuffer(dev,buf,nullptr);vkDestroyDevice(dev,nullptr);vkDestroyInstance(inst,nullptr);
    o<<"\nNext: SPIR-V compute dispatch";
    return env->NewStringUTF(o.str().c_str());
}