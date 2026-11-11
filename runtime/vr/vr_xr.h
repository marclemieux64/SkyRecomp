// OpenXR entry points used by vr.c (instance, session, frames) and vr_input.c (the controllers' actions), loaded
// from the runtime. Include only where SKYRECOMP_HAS_OPENXR is set.
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#ifdef __ANDROID__
#include <jni.h>  // (openxr_platform.h: the Android instance structures)
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define XR_INSTANCE_FUNCTIONS(X)                                                                                    \
    X(xrGetSystem) X(xrGetVulkanGraphicsRequirements2KHR) X(xrGetVulkanGraphicsDevice2KHR) X(xrCreateSession)      \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrPollEvent) X(xrCreateReferenceSpace)               \
    X(xrDestroySpace) X(xrLocateSpace) X(xrLocateViews) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)              \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)                        \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)        \
    X(xrReleaseSwapchainImage) X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction)     \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) X(xrCreateActionSpace)  \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrDestroyInstance)          \
    X(xrApplyHapticFeedback)

#define XR_DECLARE_EXTERN(name) extern PFN_##name p_##name;
XR_INSTANCE_FUNCTIONS(XR_DECLARE_EXTERN)
#undef XR_DECLARE_EXTERN

// ---- runtime/vr/vr_input.c: the controllers as the game's Joy-Con pair (buttons, sticks, motion) and their grip
// poses (wrist HUD). False: no controller input (rendering still works).
bool vr_input_create(XrInstance instance, XrSession session);
// Once per frame: sync the actions, locate both hands at `time` in `space`, hand the sample to the Joy-Con
// translator. reference: the head pose the game camera stands for ({qx, qy, qz, qw, px, py, pz}), or NULL.
void vr_input_sync(XrSpace space, XrTime time, const float reference[7]);
void vr_input_destroy(void);
