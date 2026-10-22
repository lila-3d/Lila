#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "render/VulkanContext.h"

#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Profile.h"

#include <cstdio>
#include <cstring>

namespace lila::render
{
namespace
{
std::string vkVersion(uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." + std::to_string(VK_API_VERSION_PATCH(v));
}

/** The GPU section of crash reports. */
void recordDeviceForCrashes(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& p)
{
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    char ids[64];
    std::snprintf(ids, sizeof(ids), "vendor 0x%04X device 0x%04X", p.vendorID, p.deviceID);
    crash::setContext("gpu", "name", p.deviceName);
    crash::setContext("gpu", "ids", ids);
    crash::setContext("gpu", "driver", std::string(driver.driverName) + " " + driver.driverInfo);
    crash::setContext("gpu", "driverVersion.raw", std::to_string(p.driverVersion));
    crash::setContext("gpu", "vulkan.api", vkVersion(p.apiVersion));
    crash::setContext("gpu", "vulkan.conformance", std::to_string(driver.conformanceVersion.major) + "." +
                                                      std::to_string(driver.conformanceVersion.minor) + "." +
                                                      std::to_string(driver.conformanceVersion.subminor));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log::error("vulkan: {}", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        log::warn("vulkan: {}", data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    return false;
}
} // namespace

bool VulkanContext::init(bool validation)
{
    LILA_ZONE();

    if (volkInitialize() != VK_SUCCESS)
    {
        log::error("vulkan: no Vulkan loader found; update the GPU driver");
        return false;
    }

    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef _WIN32
    exts.push_back("VK_KHR_win32_surface");
#endif
    std::vector<const char*> layers;
    validation = validation && hasLayer("VK_LAYER_KHRONOS_validation");
    if (validation)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lila";
    app.pEngineName = "Lila";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS)
    {
        log::error("vulkan: vkCreateInstance failed ({})", vkResultName(r));
        return false;
    }
    volkLoadInstance(instance);

    if (validation)
    {
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        vkCreateDebugUtilsMessengerEXT(instance, &dci, nullptr, &mMessenger);
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    // Discrete GPU with Vulkan 1.3 first.
    int best = -1;
    for (VkPhysicalDevice d : devices)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best)
        {
            best = score;
            physical = d;
            properties = p;
        }
    }
    if (!physical)
    {
        log::error("vulkan: no GPU with Vulkan 1.3");
        return false;
    }
    deviceName = properties.deviceName;
    recordDeviceForCrashes(physical, properties);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qprops.data());
    for (uint32_t i = 0; i < qn; ++i)
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            queueFamily = i;
            break;
        }

    VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
    msaaSupported = counts;
    msaa = (counts & VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT
           : (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
                                              : VK_SAMPLE_COUNT_1_BIT;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // Slang lowers discard to demote
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f13;
    f11.shaderDrawParameters = VK_TRUE; // SV_VertexID in fullscreen passes
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f11;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.wideLines = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    anisotropy = supported.samplerAnisotropy == VK_TRUE;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;

    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device); r != VK_SUCCESS)
    {
        log::error("vulkan: vkCreateDevice failed ({})", vkResultName(r));
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = physical;
    aci.device = device;
    aci.instance = instance;
    aci.pVulkanFunctions = &fns;
    vmaCreateAllocator(&aci, &allocator);

    log::info("vulkan: {} ({}x MSAA{})", deviceName, int(msaa), validation ? ", validation on" : "");
    return true;
}

void VulkanContext::shutdown()
{
    if (device)
        vkDeviceWaitIdle(device);
    if (allocator)
        vmaDestroyAllocator(allocator);
    if (device)
        vkDestroyDevice(device, nullptr);
    if (mMessenger)
        vkDestroyDebugUtilsMessengerEXT(instance, mMessenger, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    allocator = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

const char* vkResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    default: return "VkResult";
    }
}
} // namespace lila::render
