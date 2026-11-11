#include <android/log.h>
#include <android_native_app_glue.h>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SkyRecompQuest", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "SkyRecompQuest", __VA_ARGS__)

namespace {
XrInstance xr_instance = XR_NULL_HANDLE;
XrSystemId xr_system = XR_NULL_SYSTEM_ID;
XrSession xr_session = XR_NULL_HANDLE;
VkInstance vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
VkDevice vk_device = VK_NULL_HANDLE;
VkQueue vk_queue = VK_NULL_HANDLE;
VkCommandPool vk_command_pool = VK_NULL_HANDLE;
VkCommandBuffer vk_command_buffer = VK_NULL_HANDLE;
VkFence vk_fence = VK_NULL_HANDLE;
uint32_t vk_queue_family = UINT32_MAX;
XrSpace xr_local_space = XR_NULL_HANDLE;
XrSwapchain xr_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageVulkan2KHR> xr_images;
std::vector<XrView> xr_views;
std::vector<XrCompositionLayerProjectionView> xr_projection_views;
uint32_t swapchain_width = 0, swapchain_height = 0;
bool xr_session_running = false;
bool xr_attempted = false;

bool xr_ok(XrResult result, const char* operation) {
    if (XR_SUCCEEDED(result)) return true;
    LOGE("%s failed (%d)", operation, static_cast<int>(result));
    return false;
}

bool has_extension(const std::vector<XrExtensionProperties>& properties, const char* wanted) {
    for (const XrExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, wanted) == 0) return true;
    }
    return false;
}

bool vk_ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    LOGE("%s failed (%d)", operation, static_cast<int>(result));
    return false;
}

template <typename T>
bool load_xr_function(const char* name, T* function) {
    return xr_ok(xrGetInstanceProcAddr(
                     xr_instance, name,
                     reinterpret_cast<PFN_xrVoidFunction*>(function)),
                 name) && *function;
}

bool create_frame_resources() {
    vkGetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk_queue_family;
    if (!vk_ok(vkCreateCommandPool(vk_device, &pool_info, nullptr, &vk_command_pool),
               "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = vk_command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (!vk_ok(vkAllocateCommandBuffers(vk_device, &command_info, &vk_command_buffer),
               "vkAllocateCommandBuffers")) return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!vk_ok(vkCreateFence(vk_device, &fence_info, nullptr, &vk_fence),
               "vkCreateFence")) return false;

    uint32_t view_count = 0;
    if (!xr_ok(xrEnumerateViewConfigurationViews(
            xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &view_count, nullptr), "xrEnumerateViewConfigurationViews(count)") ||
        view_count != 2) {
        LOGE("Expected two primary stereo views, got %u", view_count);
        return false;
    }
    std::vector<XrViewConfigurationView> view_config(
        view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!xr_ok(xrEnumerateViewConfigurationViews(
            xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            view_count, &view_count, view_config.data()),
            "xrEnumerateViewConfigurationViews(list)")) return false;
    swapchain_width = view_config[0].recommendedImageRectWidth;
    swapchain_height = view_config[0].recommendedImageRectHeight;

    uint32_t format_count = 0;
    if (!xr_ok(xrEnumerateSwapchainFormats(xr_session, 0, &format_count, nullptr),
               "xrEnumerateSwapchainFormats(count)")) return false;
    std::vector<int64_t> formats(format_count);
    if (!xr_ok(xrEnumerateSwapchainFormats(
            xr_session, format_count, &format_count, formats.data()),
            "xrEnumerateSwapchainFormats(list)")) return false;
    int64_t selected_format = formats.empty() ? 0 : formats[0];
    for (int64_t format : formats) {
        if (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB) {
            selected_format = format;
            break;
        }
    }
    if (!selected_format) return false;

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchain_info.format = selected_format;
    swapchain_info.sampleCount = 1;
    swapchain_info.width = swapchain_width;
    swapchain_info.height = swapchain_height;
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize = view_count;
    swapchain_info.mipCount = 1;
    if (!xr_ok(xrCreateSwapchain(xr_session, &swapchain_info, &xr_swapchain),
               "xrCreateSwapchain")) return false;
    uint32_t image_count = 0;
    if (!xr_ok(xrEnumerateSwapchainImages(xr_swapchain, 0, &image_count, nullptr),
               "xrEnumerateSwapchainImages(count)")) return false;
    xr_images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (!xr_ok(xrEnumerateSwapchainImages(
            xr_swapchain, image_count, &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())),
            "xrEnumerateSwapchainImages(list)")) return false;

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xr_ok(xrCreateReferenceSpace(xr_session, &space_info, &xr_local_space),
               "xrCreateReferenceSpace")) return false;
    xr_views.assign(view_count, {XR_TYPE_VIEW});
    xr_projection_views.assign(view_count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    LOGI("Stereo smoke-test swapchain: %ux%u, %u images", swapchain_width,
         swapchain_height, image_count);
    return true;
}

bool create_vulkan_session() {
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_graphics_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
    if (!load_xr_function("xrGetVulkanGraphicsRequirements2KHR", &get_requirements) ||
        !load_xr_function("xrCreateVulkanInstanceKHR", &create_vulkan_instance) ||
        !load_xr_function("xrGetVulkanGraphicsDevice2KHR", &get_graphics_device) ||
        !load_xr_function("xrCreateVulkanDeviceKHR", &create_vulkan_device)) {
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!xr_ok(get_requirements(xr_instance, xr_system, &requirements),
               "xrGetVulkanGraphicsRequirements2KHR")) {
        return false;
    }

    const XrVersion requested_xr_api = XR_MAKE_VERSION(1, 1, 0);
    if (requested_xr_api < requirements.minApiVersionSupported ||
        requested_xr_api > requirements.maxApiVersionSupported) {
        LOGE("Vulkan 1.1 is outside the runtime range %u.%u to %u.%u",
             XR_VERSION_MAJOR(requirements.minApiVersionSupported),
             XR_VERSION_MINOR(requirements.minApiVersionSupported),
             XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
             XR_VERSION_MINOR(requirements.maxApiVersionSupported));
        return false;
    }

    VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = "Sky Recomp Quest";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "SkyRecomp";
    application_info.engineVersion = 1;
    application_info.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application_info;
    XrVulkanInstanceCreateInfoKHR xr_instance_info{
        XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_instance_info.systemId = xr_system;
    xr_instance_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xr_instance_info.vulkanCreateInfo = &instance_info;
    VkResult vk_result = VK_SUCCESS;
    if (!xr_ok(create_vulkan_instance(xr_instance, &xr_instance_info, &vk_instance,
                                      &vk_result),
               "xrCreateVulkanInstanceKHR") || vk_result != VK_SUCCESS) {
        LOGE("Vulkan instance creation failed (%d)", static_cast<int>(vk_result));
        vk_instance = VK_NULL_HANDLE;
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR device_get_info{
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_get_info.systemId = xr_system;
    device_get_info.vulkanInstance = vk_instance;
    if (!xr_ok(get_graphics_device(xr_instance, &device_get_info, &vk_physical_device),
               "xrGetVulkanGraphicsDevice2KHR")) {
        return false;
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk_physical_device, &queue_count, queues.data());
    for (uint32_t i = 0; i < queue_count; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            vk_queue_family = i;
            break;
        }
    }
    if (vk_queue_family == UINT32_MAX) {
        LOGE("The OpenXR Vulkan device has no graphics queue");
        return false;
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = vk_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    XrVulkanDeviceCreateInfoKHR xr_device_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_device_info.systemId = xr_system;
    xr_device_info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xr_device_info.vulkanPhysicalDevice = vk_physical_device;
    xr_device_info.vulkanCreateInfo = &device_info;
    vk_result = VK_SUCCESS;
    if (!xr_ok(create_vulkan_device(xr_instance, &xr_device_info, &vk_device, &vk_result),
               "xrCreateVulkanDeviceKHR") || vk_result != VK_SUCCESS) {
        LOGE("Vulkan device creation failed (%d)", static_cast<int>(vk_result));
        vk_device = VK_NULL_HANDLE;
        return false;
    }

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vk_instance;
    binding.physicalDevice = vk_physical_device;
    binding.device = vk_device;
    binding.queueFamilyIndex = vk_queue_family;
    binding.queueIndex = 0;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = xr_system;
    if (!xr_ok(xrCreateSession(xr_instance, &session_info, &xr_session),
               "xrCreateSession")) {
        xr_session = XR_NULL_HANDLE;
        return false;
    }
    LOGI("OpenXR Vulkan session created (graphics queue family %u)", vk_queue_family);
    return create_frame_resources();
}

bool render_clear_image(uint32_t image_index) {
    if (!vk_ok(vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, UINT64_MAX),
               "vkWaitForFences") ||
        !vk_ok(vkResetFences(vk_device, 1, &vk_fence), "vkResetFences") ||
        !vk_ok(vkResetCommandBuffer(vk_command_buffer, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_ok(vkBeginCommandBuffer(vk_command_buffer, &begin_info),
               "vkBeginCommandBuffer")) return false;

    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = xr_images[image_index].image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    vkCmdPipelineBarrier(vk_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_transfer);
    const VkClearColorValue colors[2] = {{{0.06f, 0.18f, 0.42f, 1.0f}},
                                         {{0.16f, 0.06f, 0.35f, 1.0f}}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
        vkCmdClearColorImage(vk_command_buffer, xr_images[image_index].image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colors[eye], 1, &range);
    }
    VkImageMemoryBarrier to_compositor = to_transfer;
    to_compositor.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_compositor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    to_compositor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_compositor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(vk_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_compositor);
    if (!vk_ok(vkEndCommandBuffer(vk_command_buffer), "vkEndCommandBuffer")) return false;
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &vk_command_buffer;
    return vk_ok(vkQueueSubmit(vk_queue, 1, &submit_info, vk_fence), "vkQueueSubmit") &&
           vk_ok(vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences(render)");
}

bool render_xr_frame() {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (!xr_ok(xrWaitFrame(xr_session, &wait_info, &frame_state), "xrWaitFrame")) return false;
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_ok(xrBeginFrame(xr_session, &begin_info), "xrBeginFrame")) return false;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* submitted_layers[1] = {};
    uint32_t layer_count = 0;
    if (frame_state.shouldRender) {
        XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
        locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate_info.displayTime = frame_state.predictedDisplayTime;
        locate_info.space = xr_local_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        uint32_t view_count = 0;
        XrResult locate_result = xrLocateViews(xr_session, &locate_info, &view_state,
                                               static_cast<uint32_t>(xr_views.size()),
                                               &view_count, xr_views.data());
        const XrViewStateFlags valid = XR_VIEW_STATE_POSITION_VALID_BIT |
                                       XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(locate_result) && view_count == 2 &&
            (view_state.viewStateFlags & valid) == valid) {
            uint32_t image_index = 0;
            XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (xr_ok(xrAcquireSwapchainImage(xr_swapchain, &acquire_info, &image_index),
                      "xrAcquireSwapchainImage")) {
                XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                image_wait.timeout = XR_INFINITE_DURATION;
                if (xr_ok(xrWaitSwapchainImage(xr_swapchain, &image_wait),
                          "xrWaitSwapchainImage") && render_clear_image(image_index)) {
                    for (uint32_t eye = 0; eye < 2; ++eye) {
                        xr_projection_views[eye].pose = xr_views[eye].pose;
                        xr_projection_views[eye].fov = xr_views[eye].fov;
                        xr_projection_views[eye].subImage.swapchain = xr_swapchain;
                        xr_projection_views[eye].subImage.imageRect = {
                            {0, 0}, {static_cast<int32_t>(swapchain_width),
                                     static_cast<int32_t>(swapchain_height)}};
                        xr_projection_views[eye].subImage.imageArrayIndex = eye;
                    }
                    layer.space = xr_local_space;
                    layer.viewCount = 2;
                    layer.views = xr_projection_views.data();
                    submitted_layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
                    layer_count = 1;
                }
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(xr_swapchain, &release_info);
            }
        }
    }
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = layer_count;
    end_info.layers = layer_count ? submitted_layers : nullptr;
    return xr_ok(xrEndFrame(xr_session, &end_info), "xrEndFrame");
}

bool initialize_openxr(android_app* app) {
    if (xr_instance != XR_NULL_HANDLE) return true;
    if (xr_attempted) return false;
    xr_attempted = true;

    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    XrResult result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (!xr_ok(result, "xrGetInstanceProcAddr(xrInitializeLoaderKHR)") || !initialize_loader) {
        LOGE("The packaged OpenXR loader has no Android initialization entry point");
        return false;
    }

    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = app->activity->vm;
    loader_info.applicationContext = app->activity->clazz;
    if (!xr_ok(initialize_loader(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info)),
            "xrInitializeLoaderKHR")) {
        return false;
    }

    uint32_t extension_count = 0;
    if (!xr_ok(xrEnumerateInstanceExtensionProperties(
            nullptr, 0, &extension_count, nullptr),
            "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(
        extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!xr_ok(xrEnumerateInstanceExtensionProperties(
            nullptr, extension_count, &extension_count, extensions.data()),
            "xrEnumerateInstanceExtensionProperties(list)")) {
        return false;
    }

    constexpr const char* required_extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    };
    for (const char* required : required_extensions) {
        if (!has_extension(extensions, required)) {
            LOGE("OpenXR runtime is missing required extension %s", required);
            return false;
        }
    }

    XrInstanceCreateInfoAndroidKHR android_info{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = app->activity->vm;
    android_info.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    create_info.next = &android_info;
    create_info.enabledExtensionCount =
        static_cast<uint32_t>(sizeof(required_extensions) / sizeof(required_extensions[0]));
    create_info.enabledExtensionNames = required_extensions;
    std::strncpy(create_info.applicationInfo.applicationName, "Sky Recomp Quest",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName, "SkyRecomp",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.applicationVersion = 1;
    create_info.applicationInfo.engineVersion = 1;
    create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!xr_ok(xrCreateInstance(&create_info, &xr_instance), "xrCreateInstance")) {
        xr_instance = XR_NULL_HANDLE;
        return false;
    }

    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(xr_instance, &instance_properties))) {
        LOGI("OpenXR runtime: %s (%u.%u.%u)", instance_properties.runtimeName,
             XR_VERSION_MAJOR(instance_properties.runtimeVersion),
             XR_VERSION_MINOR(instance_properties.runtimeVersion),
             XR_VERSION_PATCH(instance_properties.runtimeVersion));
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_ok(xrGetSystem(xr_instance, &system_info, &xr_system), "xrGetSystem(HMD)")) {
        xrDestroyInstance(xr_instance);
        xr_instance = XR_NULL_HANDLE;
        xr_system = XR_NULL_SYSTEM_ID;
        return false;
    }

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(xr_instance, xr_system, &system_properties))) {
        LOGI("OpenXR HMD: %s, max swapchain %ux%u", system_properties.systemName,
             system_properties.graphicsProperties.maxSwapchainImageWidth,
             system_properties.graphicsProperties.maxSwapchainImageHeight);
    }
    return create_vulkan_session();
}

void poll_xr_events(android_app* app) {
    if (xr_instance == XR_NULL_HANDLE) return;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            LOGI("OpenXR session state changed to %d", static_cast<int>(changed->state));
            if (changed->state == XR_SESSION_STATE_READY && !xr_session_running) {
                XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                begin_info.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xr_ok(xrBeginSession(xr_session, &begin_info), "xrBeginSession"))
                    xr_session_running = true;
            } else if (changed->state == XR_SESSION_STATE_STOPPING && xr_session_running) {
                xrEndSession(xr_session);
                xr_session_running = false;
            } else if (changed->state == XR_SESSION_STATE_EXITING ||
                       changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                ANativeActivity_finish(app->activity);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            ANativeActivity_finish(app->activity);
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void shutdown_openxr() {
    xr_session_running = false;
    if (vk_device != VK_NULL_HANDLE) vkDeviceWaitIdle(vk_device);
    if (xr_local_space != XR_NULL_HANDLE) {
        xrDestroySpace(xr_local_space);
        xr_local_space = XR_NULL_HANDLE;
    }
    if (xr_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(xr_swapchain);
        xr_swapchain = XR_NULL_HANDLE;
    }
    xr_images.clear();
    xr_views.clear();
    xr_projection_views.clear();
    if (vk_fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk_device, vk_fence, nullptr);
        vk_fence = VK_NULL_HANDLE;
    }
    if (vk_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk_device, vk_command_pool, nullptr);
        vk_command_pool = VK_NULL_HANDLE;
        vk_command_buffer = VK_NULL_HANDLE;
    }
    if (xr_session != XR_NULL_HANDLE) {
        xrDestroySession(xr_session);
        xr_session = XR_NULL_HANDLE;
    }
    if (vk_device != VK_NULL_HANDLE) {
        vkDestroyDevice(vk_device, nullptr);
        vk_device = VK_NULL_HANDLE;
    }
    vk_physical_device = VK_NULL_HANDLE;
    if (vk_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(vk_instance, nullptr);
        vk_instance = VK_NULL_HANDLE;
    }
    vk_queue_family = UINT32_MAX;
    xr_system = XR_NULL_SYSTEM_ID;
    if (xr_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(xr_instance);
        xr_instance = XR_NULL_HANDLE;
        LOGI("OpenXR instance destroyed");
    }
}

void on_app_command(android_app* app, int32_t command) {
    switch (command) {
    case APP_CMD_RESUME:
        LOGI("Activity resumed; initializing Android OpenXR");
        initialize_openxr(app);
        break;
    case APP_CMD_PAUSE:
        LOGI("Activity paused");
        break;
    case APP_CMD_DESTROY:
        shutdown_openxr();
        LOGI("Activity destroyed");
        break;
    default:
        break;
    }
}
} // namespace

extern "C" void android_main(android_app* app) {
    app->onAppCmd = on_app_command;
    LOGI("Sky Recomp Quest base started (OpenXR %u.%u.%u headers)",
         XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),
         XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
         XR_VERSION_PATCH(XR_CURRENT_API_VERSION));

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int timeout = xr_session_running ? 0 : -1;
        const int result = ALooper_pollOnce(timeout, nullptr, &events,
                                            reinterpret_cast<void**>(&source));
        if (result >= 0 && source) source->process(app, source);
        poll_xr_events(app);
        if (xr_session_running && !render_xr_frame()) ANativeActivity_finish(app->activity);
    }
    shutdown_openxr();
}
