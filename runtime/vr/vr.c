// Optional OpenXR / SteamVR output: instance, session, stereo swapchain, frame loop, headset and controller poses.
// XR owns no desktop renderer state; with VR disabled (the default) none of the loader / runtime code runs.
//
// Threading: everything after vr_vulkan_init runs on the renderer thread (backend_execute / backend_present),
// so the frame state below needs no locks. Only vr_debug_stage is read from another thread (the freeze watchdog).
#include "vr.h"
#include "vr_internal.h"
#include "vr_fade.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "hle.h"
#include "settings.h"

// (SKYRECOMP_NO_OPENXR: build without it even where the SDK is installed)
#if !defined(__APPLE__) && !defined(SKYRECOMP_NO_OPENXR) && __has_include(<openxr/openxr.h>) && \
    __has_include(<openxr/openxr_platform.h>)
#define SKYRECOMP_HAS_OPENXR 1
#include "vr_xr.h"
#include "nvn/vulkan/StereoCameraHook.h"
#include "vr_rig.h"
#include "vr_settings.h"
#include "vr_stereo_cull.h"
#include "vr_hud.h"
#include "nvn/vulkan/shaders.h"
#else
#define SKYRECOMP_HAS_OPENXR 0
#endif

static bool available, running;
static atomic_bool game_active;
void vr_set_game_active(bool active) { atomic_store(&game_active, active); }
bool vr_game_active(void) { return atomic_load(&game_active); }
float g_vr_gpu_ms;  // the last measured GPU frame time (exec.c), for dynamic resolution
static atomic_bool focus_requested;  // set on the renderer thread, taken on the UI thread
static char status_text[192] = "VR is disabled";

enum { STAGE_IDLE, STAGE_WAIT_FRAME, STAGE_BEGIN_FRAME, STAGE_RENDER, STAGE_ACQUIRE, STAGE_WAIT_IMAGE, STAGE_END_FRAME };
static _Atomic int debug_stage;
static uint64_t stage_ns[7], stage_t0;  // time spent in each stage (SKYRECOMP_VR_PACE_TRACE)
static void set_stage(int stage) {
    const uint64_t now = SDL_GetTicksNS();
    const int prev = atomic_exchange(&debug_stage, stage);
    if (stage_t0 && prev >= 0 && prev < 7) stage_ns[prev] += now - stage_t0;
    stage_t0 = now;
}

const char *vr_debug_stage(void) {
    static const char *const names[] = {"idle", "xrWaitFrame", "xrBeginFrame", "rendering", "xrAcquireSwapchainImage",
                                        "xrWaitSwapchainImage", "xrEndFrame"};
    int stage = atomic_load(&debug_stage);
    return stage >= 0 && stage < (int)(sizeof names / sizeof *names) ? names[stage] : "unknown";
}

bool vr_requested(void) {
    const char *override = getenv("SKYRECOMP_VR");
    return override ? atoi(override) != 0 : g_settings.vr;
}
bool vr_available(void) { return available; }
bool vr_running(void) { return running; }
const char *vr_status(void) { return status_text; }

static atomic_bool g_exit_requested;
void TriggerFastVRExit(void) { atomic_store(&g_exit_requested, true); }
bool IsVRExitRequested(void) { return atomic_load(&g_exit_requested); }

bool vr_take_window_focus_request(void) { return atomic_exchange(&focus_requested, false); }

static void set_status(const char *text) {
    snprintf(status_text, sizeof status_text, "%s", text);
    LOG("[vr] %s\n", status_text);
}

#if SKYRECOMP_HAS_OPENXR

// ================================================================== OpenXR entry points (vr_xr.h)
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static PFN_xrCreateInstance p_xrCreateInstance;
static PFN_xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties;
#define XR_DECLARE(name) PFN_##name p_##name;
XR_INSTANCE_FUNCTIONS(XR_DECLARE)
#undef XR_DECLARE

// ================================================================== state
static void *loader;
static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system = XR_NULL_SYSTEM_ID;
static XrSession xr_session = XR_NULL_HANDLE;
static XrSpace local_space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;

static VkDevice vk_device;
static VkPhysicalDevice vk_physical_device;
static VkImage staging;              // (create_staging)
static VkDeviceMemory staging_memory;
// The desktop mirror samples the very images the compositor gets (vr_mirror_layers).
#define MIRROR_USAGE XR_SWAPCHAIN_USAGE_SAMPLED_BIT
static XrSwapchain swapchain = XR_NULL_HANDLE;  // one image per frame, array layer = eye
static VkFormat swapchain_format;
static uint32_t eye_width, eye_height, image_count;
// The headset's eye image: width / height (0 before the session) - exec.c: 2D drawn into the eyes keeps its shape
float vr_eye_aspect(void) { return eye_height ? (float)eye_width / (float)eye_height : 0.0f; }
static XrSwapchainImageVulkanKHR *images;
static XrSwapchain view_fade_swapchain = XR_NULL_HANDLE;
static XrSwapchainImageVulkanKHR *view_fade_images;
static uint32_t view_fade_count, view_fade_index;
static bool view_fade_acquired, view_fade_ready, view_fade_failed;
static float adaptive_resolution;
static double adaptive_work_sum, adaptive_budget_sum;
static int adaptive_samples, adaptive_headroom;

static float configured_resolution_ceiling(void) {
    static bool read;
    if (!read) {
        read = true;
        const char *value = getenv("SKYRECOMP_VR_SCALE");
        if (value) {
            const float parsed = (float)atof(value);
            if (isfinite(parsed) && parsed >= 0.5f && parsed <= 2.0f) {
                g_vr_config.resolution_scale = parsed;
                LOG("[vr] initial internal render scale from SKYRECOMP_VR_SCALE: %.3fx (VR Options remains live)\n", parsed);
            }
            else if (!isfinite(parsed) || parsed < 10.0f || parsed > 1000.0f)
                LOG("[vr] ignoring SKYRECOMP_VR_SCALE=%s (render scale expects 0.5..2.0)\n", value);
        }
    }
    return SDL_clamp(g_vr_config.resolution_scale, 0.5f, 2.0f);
}

float vr_resolution_factor(void) {
    float ceiling = configured_resolution_ceiling();
    float floor = SDL_clamp(g_vr_config.dynamic_resolution_min, 0.5f, ceiling);
    if (!g_vr_config.dynamic_resolution) return ceiling;
    if (!isfinite(adaptive_resolution) || adaptive_resolution <= 0) adaptive_resolution = ceiling;
    adaptive_resolution = SDL_clamp(adaptive_resolution, floor, ceiling);
    return adaptive_resolution;
}

bool vr_eye_extent(uint32_t *w, uint32_t *h) {  // the headset's eye image size (the swapchain's)
    if (!eye_width || !eye_height) return false;
    *w = eye_width; *h = eye_height;
    return true;
}

bool vr_render_scale(float *sx, float *sy) {
    if (!eye_width || !eye_height || !sx || !sy) return false;
    float scale = vr_resolution_factor();
    *sx = (float)eye_width / 1920.0f * scale;
    *sy = (float)eye_height / 1080.0f * scale;
    return true;
}

static float reference_pose[7];         // head pose the game camera stands for (set on the first tracked frame)
static bool reference_valid;
// UI consumes the same raw grip samples as the controller renderer, but never feeds them back into Link or vr_rig.
static bool hud_grip_tracked[2];
static XrPosef hud_grip_pose[2];

void vr_set_grip_poses(bool left_tracked, const float left[7], bool right_tracked, const float right[7]) {
    vr_rig_set_grips(left_tracked, left, right_tracked, right);  // (Link's hands on the controllers)
    const bool tracked[2] = {left_tracked, right_tracked};
    const float *pose[2] = {left, right};
    for (int h = 0; h < 2; h++) {
        hud_grip_tracked[h] = tracked[h] && pose[h];
        if (hud_grip_tracked[h]) {
            hud_grip_pose[h].orientation = (XrQuaternionf){pose[h][0], pose[h][1], pose[h][2], pose[h][3]};
            hud_grip_pose[h].position = (XrVector3f){pose[h][4], pose[h][5], pose[h][6]};
        }
    }
}

static struct {
    bool begun;     // xrBeginFrame succeeded; xrEndFrame is owed
    bool stereo;    // shouldRender and both eye views tracked: the renderer records both eyes
    bool acquired;  // a swapchain image was acquired and waited; xrReleaseSwapchainImage is owed
    bool copied;    // both eye layers were recorded: submit the projection layer
    uint32_t image;
    bool hud_acquired, hud_copied;  // the HUD panel's swapchain image: acquired / recorded (submit the quad layer)
    bool hud_opaque;                // full-screen 2D: the panel opaque, in front of the frozen world
    bool frozen_used;               // the eyes show the frozen world (vr_freeze_world): its poses go in the layer
    uint32_t hud_image;
    uint32_t patched[2];  // draws whose scene camera became the eye's (trace)
    XrTime display_time;
    XrDuration display_period;
    uint64_t work_start_ns;
    XrView views[2];
} frame;

bool vr_eye_fov(int eye, float fov[4]) {  // this frame's eye field of view: tangent angles left, right, up, down (radians)
    if (!frame.begun || !frame.stereo || eye < 0 || eye > 1) return false;
    const XrFovf *f = &frame.views[eye].fov;
    fov[0] = f->angleLeft; fov[1] = f->angleRight; fov[2] = f->angleUp; fov[3] = f->angleDown;
    return true;
}


static void hud_destroy(void);
static void view_fade_destroy(void);
static void menu_destroy(void);
static bool mirror_quad_mvp(const float m[16], float hx, float hy, float out[16]);
static bool eye_quad_mvp(int eye, const float m[16], float hx, float hy, float out[16]);
static void frozen_destroy(void);
extern bool vk_hud_layer_enabled(void);  // (exec.c: SKYRECOMP_VR_HUD_LAYER)

static bool trace_enabled(void) {
    static int on = -1;
    if (on < 0) on = getenv("SKYRECOMP_VR_TRACE") != NULL;
    return on;
}

static bool xr_ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    char text[160];
    snprintf(text, sizeof text, "%s failed (%d)", what, (int)r);
    set_status(text);
    return false;
}

// Per-frame calls: a persistent failure is logged a few times, not every frame.
static bool xr_frame_ok(XrResult r, const char *what, int *logged) {
    if (XR_SUCCEEDED(r)) return true;
    if ((*logged)++ < 5) LOG("[vr] %s failed (%d)\n", what, (int)r);
    return false;
}

static void update_cull(void);                  // (frame loop below)

// ================================================================== instance
static bool load_global(const char *name, PFN_xrVoidFunction *out) {
    *out = (PFN_xrVoidFunction)SDL_LoadFunction(loader, name);
    if (*out) return true;
    snprintf(status_text, sizeof status_text, "OpenXR loader is missing %s", name);
    LOG("[vr] %s\n", status_text);
    return false;
}

static bool load_instance_functions(void) {
#define XR_LOAD(name)                                                                                    \
    if (XR_FAILED(p_xrGetInstanceProcAddr(xr_instance, #name, (PFN_xrVoidFunction *)&p_##name)) || !p_##name) { \
        set_status("OpenXR runtime is missing " #name);                                                 \
        return false;                                                                                   \
    }
    XR_INSTANCE_FUNCTIONS(XR_LOAD)
#undef XR_LOAD
    return true;
}

// ---- the headset's hidden area (XR_KHR_visibility_mask): per eye, triangles in view space on the z = -1 plane
// (tangents of the view angles), the parts of the eye image the lenses / faceplate never show.
static bool has_visibility_mask;
static float *hidden_xy[2];  // (x, y) per vertex, 3 per triangle
static uint32_t hidden_n[2];

static void hidden_mesh_load(void) {
    PFN_xrGetVisibilityMaskKHR get = NULL;
    if (!has_visibility_mask || XR_FAILED(p_xrGetInstanceProcAddr(xr_instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&get)) || !get)
        return;
    for (uint32_t eye = 0; eye < 2; eye++) {
        XrVisibilityMaskKHR m = {.type = XR_TYPE_VISIBILITY_MASK_KHR};
        if (XR_FAILED(get(xr_session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m)) ||
            !m.vertexCountOutput || !m.indexCountOutput)
            continue;
        XrVector2f *v = SDL_calloc(m.vertexCountOutput, sizeof *v);
        uint32_t *idx = SDL_calloc(m.indexCountOutput, sizeof *idx);
        m.vertexCapacityInput = m.vertexCountOutput; m.vertices = v;
        m.indexCapacityInput = m.indexCountOutput; m.indices = idx;
        if (XR_SUCCEEDED(get(xr_session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m))) {
            SDL_free(hidden_xy[eye]);
            hidden_xy[eye] = SDL_calloc(m.indexCountOutput * 2, sizeof(float));
            for (uint32_t i = 0; i < m.indexCountOutput; i++) {
                const XrVector2f p = idx[i] < m.vertexCountOutput ? v[idx[i]] : (XrVector2f){0, 0};
                hidden_xy[eye][i * 2] = p.x; hidden_xy[eye][i * 2 + 1] = p.y;
            }
            hidden_n[eye] = m.indexCountOutput;
        }
        SDL_free(v);
        SDL_free(idx);
    }
    LOG("[vr] hidden area meshes: %u / %u triangles\n", hidden_n[0] / 3, hidden_n[1] / 3);
}

// Eye `eye`'s hidden-area triangles (x, y pairs, 3 vertices each, view-space tangents); 0 when the runtime has none.
uint32_t vr_hidden_mesh(int eye, const float **xy) {
    if (eye < 0 || eye > 1 || !hidden_n[eye]) return 0;
    *xy = hidden_xy[eye];
    return hidden_n[eye];
}

static bool runtime_has_extension(const char *wanted) {
    uint32_t count = 0;
    if (!xr_ok(p_xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL), "xrEnumerateInstanceExtensionProperties") ||
        !count || count > 4096)
        return false;
    XrExtensionProperties *props = SDL_calloc(count, sizeof *props);
    if (!props) return false;
    for (uint32_t i = 0; i < count; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    bool found = false;
    if (xr_ok(p_xrEnumerateInstanceExtensionProperties(NULL, count, &count, props), "xrEnumerateInstanceExtensionProperties"))
        for (uint32_t i = 0; i < count && !found; i++) found = !strcmp(props[i].extensionName, wanted);
    SDL_free(props);
    return found;
}

static bool create_instance(void) {
#ifdef _WIN32
    loader = SDL_LoadObject("openxr_loader.dll");
    if (!loader) loader = SDL_LoadObject("libopenxr_loader.dll");  // (MSYS2 package name)
#else
    loader = SDL_LoadObject("libopenxr_loader.so.1");
    if (!loader) loader = SDL_LoadObject("libopenxr_loader.so");
#endif
    if (!loader) {
        snprintf(status_text, sizeof status_text, "OpenXR loader not found: %s", SDL_GetError());
        LOG("[vr] %s\n", status_text);
        return false;
    }
    if (!load_global("xrGetInstanceProcAddr", (PFN_xrVoidFunction *)&p_xrGetInstanceProcAddr) ||
        !load_global("xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance) ||
        !load_global("xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&p_xrEnumerateInstanceExtensionProperties))
        return false;
#ifdef __ANDROID__
    // Android (Meta Quest): the loader needs the Java VM and the activity before anything else, and so does the
    // instance. SDL's activity started this process.
    JNIEnv *jni = SDL_GetAndroidJNIEnv();
    JavaVM *vm = NULL;
    jobject activity = SDL_GetAndroidActivity();
    if (!jni || (*jni)->GetJavaVM(jni, &vm) != JNI_OK || !activity) {
        set_status("No Android activity for OpenXR");
        return false;
    }
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    XrLoaderInitInfoAndroidKHR li = {.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, .applicationVM = vm,
                                     .applicationContext = activity};
    if (!load_global("xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader) ||
        !xr_ok(init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li), "xrInitializeLoaderKHR"))
        return false;
#endif
    if (!runtime_has_extension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        set_status("The active OpenXR runtime does not support Vulkan");
        return false;
    }
    // XR_KHR_visibility_mask: the parts of each eye image the headset hides (hidden_mesh_load)
    has_visibility_mask = runtime_has_extension(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
    const char *extensions[3] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t n_extensions = 1;
    if (has_visibility_mask) extensions[n_extensions++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionNames = extensions};
#ifdef __ANDROID__
    extensions[n_extensions++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    XrInstanceCreateInfoAndroidKHR android_info = {.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, .applicationVM = vm,
                                                   .applicationActivity = activity};
    ci.next = &android_info;
#endif
    ci.enabledExtensionCount = n_extensions;
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Sky Recomp");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "Sky Recomp");
    ci.applicationInfo.applicationVersion = ci.applicationInfo.engineVersion = 1;
    // SteamVR advertises OpenXR 1.0 where newer SDK headers default to 1.1; everything used here is core 1.0 plus
    // XR_KHR_vulkan_enable2.
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!xr_ok(p_xrCreateInstance(&ci, &xr_instance), "xrCreateInstance") || !load_instance_functions()) return false;
    XrSystemGetInfo gi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    return xr_ok(p_xrGetSystem(xr_instance, &gi, &xr_system), "xrGetSystem (start SteamVR and connect a headset)");
}

// ================================================================== for the gameplay modules (vr_internal.h)
static void store_pose(float out[7], const XrPosef *p);
const float *vr_reference_pose(void) { return reference_pose; }
bool vr_eye_pose(int eye, float pose[7]) {
    if (eye < 0 || eye > 1) return false;
    store_pose(pose, &frame.views[eye].pose);
    return true;
}
bool vr_eye_tangents(int eye, float t[4]) {
    if (eye < 0 || eye > 1) return false;
    const XrFovf *f = &frame.views[eye].fov;
    t[0] = tanf(f->angleLeft), t[1] = tanf(f->angleRight), t[2] = tanf(f->angleUp), t[3] = tanf(f->angleDown);
    return true;
}

// ================================================================== stereo swapchain
static bool create_swapchain(void) {
    uint32_t nviews = 0;
    XrViewConfigurationView views[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!xr_ok(p_xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   0, &nviews, NULL), "xrEnumerateViewConfigurationViews") ||
        nviews != 2 ||
        !xr_ok(p_xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   2, &nviews, views), "xrEnumerateViewConfigurationViews")) {
        set_status("OpenXR stereo views are unavailable");
        return false;
    }
    // One array swapchain for both eyes: the same size per layer, so the larger recommendation of the two.
    eye_width = SDL_max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
    eye_height = SDL_max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);

    uint32_t nformats = 0;
    if (!xr_ok(p_xrEnumerateSwapchainFormats(xr_session, 0, &nformats, NULL), "xrEnumerateSwapchainFormats") || !nformats)
        return false;
    int64_t *formats = SDL_calloc(nformats, sizeof *formats);
    if (!formats) return false;
    bool ok = xr_ok(p_xrEnumerateSwapchainFormats(xr_session, nformats, &nformats, formats), "xrEnumerateSwapchainFormats");
    // sRGB first: the game's presented image holds display-referred values, and an sRGB swapchain is what the
    // compositor expects for them.
    static const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_FORMAT_B8G8R8A8_UNORM};
    swapchain_format = VK_FORMAT_UNDEFINED;
    for (uint32_t p = 0; ok && p < SDL_arraysize(preferred) && swapchain_format == VK_FORMAT_UNDEFINED; p++)
        for (uint32_t i = 0; i < nformats; i++)
            if (formats[i] == preferred[p]) { swapchain_format = preferred[p]; break; }
    SDL_free(formats);
    if (swapchain_format == VK_FORMAT_UNDEFINED) {
        set_status("SteamVR offers no compatible RGBA eye format");
        return false;
    }

    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | MIRROR_USAGE,
                                .format = swapchain_format, .sampleCount = 1, .width = eye_width, .height = eye_height,
                                .faceCount = 1, .arraySize = 2, .mipCount = 1};
    if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &swapchain), "xrCreateSwapchain") ||
        !xr_ok(p_xrEnumerateSwapchainImages(swapchain, 0, &image_count, NULL), "xrEnumerateSwapchainImages") || !image_count)
        return false;
    images = SDL_calloc(image_count, sizeof *images);
    if (!images) return false;
    for (uint32_t i = 0; i < image_count; i++) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!xr_ok(p_xrEnumerateSwapchainImages(swapchain, image_count, &image_count, (XrSwapchainImageBaseHeader *)images),
               "xrEnumerateSwapchainImages"))
        return false;
    LOG("[vr] stereo swapchain: %ux%u per eye, 2 array layers, %u images, format %d\n", eye_width, eye_height,
        image_count, swapchain_format);
    return true;
}


// The UNORM image with the swapchain's channel order the eyes are blitted into when the game image is not sRGB
// (vr_record_eye_copies). None for a linear swapchain.
static bool create_staging(void) {
    VkFormat f = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
    if (f == VK_FORMAT_UNDEFINED) return true;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {eye_width, eye_height, 1}, .mipLevels = 1, .arrayLayers = 2,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(vk_device, &ci, NULL, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_device, staging, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    return type != UINT32_MAX && vkAllocateMemory(vk_device, &ai, NULL, &staging_memory) == VK_SUCCESS &&
           vkBindImageMemory(vk_device, staging, staging_memory, 0) == VK_SUCCESS;
}
// ================================================================== init / shutdown
bool vr_vulkan_init(void *instance_, void *physical_device_, void *device_, uint32_t queue_family, uint32_t queue_index) {
    if (!vr_requested()) return false;
    if (!instance_ || !physical_device_ || !device_) return false;
    if (loader || xr_instance) vr_shutdown();
    set_status("Starting OpenXR");
    if (!create_instance()) {
        vr_shutdown();
        return false;
    }
    VkInstance instance = instance_;
    VkPhysicalDevice physical_device = physical_device_;
    vk_device = device_;
    vk_physical_device = physical_device_;

    XrGraphicsRequirementsVulkanKHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!xr_ok(p_xrGetVulkanGraphicsRequirements2KHR(xr_instance, xr_system, &req), "xrGetVulkanGraphicsRequirements2KHR")) {
        vr_shutdown();
        return false;
    }
    const XrVersion vulkan_version = XR_MAKE_VERSION(1, 3, 0);
    if (vulkan_version < req.minApiVersionSupported) {
        set_status("The OpenXR runtime requires a newer Vulkan version");
        vr_shutdown();
        return false;
    }
    if (vulkan_version > req.maxApiVersionSupported)
        LOG("[vr] runtime reports Vulkan max %u.%u; using the existing Vulkan 1.3 device\n",
            XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    XrVulkanGraphicsDeviceGetInfoKHR dgi = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, .systemId = xr_system,
                                            .vulkanInstance = instance};
    VkPhysicalDevice runtime_device = VK_NULL_HANDLE;
    if (!xr_ok(p_xrGetVulkanGraphicsDevice2KHR(xr_instance, &dgi, &runtime_device), "xrGetVulkanGraphicsDevice2KHR") ||
        runtime_device != physical_device) {
        set_status("SteamVR requires a different GPU; select it with LARECOMP_GPU");
        vr_shutdown();
        return false;
    }
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR, .instance = instance,
                                           .physicalDevice = physical_device, .device = vk_device,
                                           .queueFamilyIndex = queue_family, .queueIndex = queue_index};
    XrSessionCreateInfo sci = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = xr_system};
    XrReferenceSpaceCreateInfo space = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                        .poseInReferenceSpace.orientation.w = 1.0f};
    bool ok = xr_ok(p_xrCreateSession(xr_instance, &sci, &xr_session), "xrCreateSession") &&
              xr_ok(p_xrCreateReferenceSpace(xr_session, &space, &local_space), "xrCreateReferenceSpace (local)");
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    ok = ok && xr_ok(p_xrCreateReferenceSpace(xr_session, &space, &view_space), "xrCreateReferenceSpace (view)") &&
         create_swapchain() && create_staging();
    if (ok && !vr_input_create(xr_instance, xr_session))
        LOG("[vr] no controller input (the headset still renders)\n");
    if (!ok) {
        vr_shutdown();
        return false;
    }
    available = true;
    hidden_mesh_load();
    set_status("SteamVR connected; waiting for the headset");
    return true;
}

void vr_shutdown(void) {
    if (frame.begun) vr_frame_end();
    running = available = reference_valid = false;
    { extern int g_rt_headset_hz; g_rt_headset_hz = 0; }  // (frame_rate.c: back to the chosen target)
    atomic_store(&focus_requested, false);
    memset(&frame, 0, sizeof frame);
    set_stage(STAGE_IDLE);
    vr_input_destroy();
    if (vk_device && staging) vkDestroyImage(vk_device, staging, NULL);
    if (vk_device && staging_memory) vkFreeMemory(vk_device, staging_memory, NULL);
    staging = VK_NULL_HANDLE;
    staging_memory = VK_NULL_HANDLE;
    SDL_free(images);
    images = NULL;
    image_count = 0;
    if (swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(swapchain);
    swapchain = XR_NULL_HANDLE;
    hud_destroy();
    view_fade_destroy();
    menu_destroy();
    frozen_destroy();
    XrSpace *spaces[] = {&view_space, &local_space};
    for (size_t i = 0; i < SDL_arraysize(spaces); i++) {
        if (*spaces[i] && p_xrDestroySpace) p_xrDestroySpace(*spaces[i]);
        *spaces[i] = XR_NULL_HANDLE;
    }
    if (xr_session && p_xrDestroySession) p_xrDestroySession(xr_session);
    if (xr_instance && p_xrDestroyInstance) p_xrDestroyInstance(xr_instance);
    xr_session = XR_NULL_HANDLE;
    xr_instance = XR_NULL_HANDLE;
    xr_system = XR_NULL_SYSTEM_ID;
    vk_device = VK_NULL_HANDLE;
    if (loader) SDL_UnloadObject(loader);
    loader = NULL;
}

// ================================================================== session events
void vr_poll(void) {
    if (!available) return;
    XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(xr_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrSessionState state = ((const XrEventDataSessionStateChanged *)&event)->state;
            LOG("[vr] session state %d\n", (int)state);
            if (state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo begin = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                            .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (xr_ok(p_xrBeginSession(xr_session, &begin), "xrBeginSession")) {
                    running = true;
                    reference_valid = false;
                    extern void input_controller_style_changed(void);
                    input_controller_style_changed();  // (the game opens the Joy-Con pair's motion sensors)
                    set_status("SteamVR stereo rendering active");
                }
            } else if (state == XR_SESSION_STATE_FOCUSED) {
                atomic_store(&focus_requested, true);
            } else if (state == XR_SESSION_STATE_STOPPING && running) {
                if (frame.begun) vr_frame_end();
                p_xrEndSession(xr_session);
                running = false;
                extern void input_controller_style_changed(void);
                input_controller_style_changed();
                set_status("SteamVR session stopped");
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                running = available = false;
                set_status("SteamVR session ended");
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            vr_recenter();  // (SteamVR recentred)
        }
        event = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
    // Controllers are located in the same OpenXR local space as the HMD (the game's Joy-Cons, the wrist HUD).
    // Between frames (the game waiting, e.g. on the name entry dialog: only re-presents, no frame begun yet) the
    // last frame's display time: with 0 the controllers aren't synced at all (no menu pointer).
    static XrTime last_time;
    if (frame.display_time) last_time = frame.display_time;
    vr_input_sync(running ? local_space : XR_NULL_HANDLE, last_time, reference_valid ? reference_pose : NULL);
}

// ================================================================== frame loop
// The runtime's display lead (predicted display time - wall clock at xrBeginFrame), averaged: the game thread places
// the menu pointer by it (vr_display_time_now), wherever the render thread is meanwhile.
static pthread_mutex_t timing_lock = PTHREAD_MUTEX_INITIALIZER;
static double timing_lead;
static bool timing_lead_ok;
// The display time of a frame begun now: the wall clock plus the runtime's display lead, averaged over frames
// (OpenXR time is in nanoseconds, as SDL's clock). Evenly spaced reads get evenly spaced times.
int64_t vr_display_time_now(void) {
    pthread_mutex_lock(&timing_lock);
    const int64_t t = timing_lead_ok ? (int64_t)SDL_GetTicksNS() + (int64_t)timing_lead : 0;
    pthread_mutex_unlock(&timing_lock);
    return t;
}

static void store_pose(float out[7], const XrPosef *p) {
    out[0] = p->orientation.x; out[1] = p->orientation.y; out[2] = p->orientation.z; out[3] = p->orientation.w;
    out[4] = p->position.x; out[5] = p->position.y; out[6] = p->position.z;
}

// Turning (VR Options > Turning, vr_input.c): the reference head pose - the game camera's stand-in - turns about the
// vertical through the head, so the world turns about the player. Radians, counter-clockwise from above.
static _Atomic float g_turn_pending;
void vr_turn(float radians) {
    float v = atomic_load(&g_turn_pending);
    while (!atomic_compare_exchange_weak(&g_turn_pending, &v, v + radians)) {}
}
static void apply_turn(void) {
    const float d = atomic_exchange(&g_turn_pending, 0.0f);
    if (d == 0.0f || !reference_valid) return;
    const float hx = (frame.views[0].pose.position.x + frame.views[1].pose.position.x) * 0.5f;
    const float hz = (frame.views[0].pose.position.z + frame.views[1].pose.position.z) * 0.5f;
    const float s = sinf(d * 0.5f), c = cosf(d * 0.5f);  // yaw(d) * reference (yaw only: x = z = 0)
    const float qy = reference_pose[1], qw = reference_pose[3];
    reference_pose[1] = qy * c + qw * s;
    reference_pose[3] = qw * c - qy * s;
    const float sd = sinf(d), cd = cosf(d), x = reference_pose[4] - hx, z = reference_pose[6] - hz;
    reference_pose[4] = hx + x * cd + z * sd;
    reference_pose[6] = hz - x * sd + z * cd;
}

static bool locate_views(void) {
    static int logged;
    const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (!reference_valid) {
        XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(p_xrLocateSpace(view_space, local_space, frame.display_time, &head)) &&
            (head.locationFlags & tracked) == tracked) {
            store_pose(reference_pose, &head.pose);
            vr_rig_yaw_only(reference_pose, reference_pose);  // (yaw only: a tilted head at the start tilts nothing)
            reference_valid = true;
        }
    }
    XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                           .displayTime = frame.display_time, .space = local_space};
    XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
    frame.views[0] = frame.views[1] = (XrView){.type = XR_TYPE_VIEW};
    uint32_t count = 0;
    const XrViewStateFlags valid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    const bool located = xr_frame_ok(p_xrLocateViews(xr_session, &li, &vs, 2, &count, frame.views), "xrLocateViews", &logged);
    const bool pose_valid = located && count == 2 && (vs.viewStateFlags & valid) == valid;
    const bool ok = pose_valid && reference_valid;
    if (ok) apply_turn();
    // (test) SKYRECOMP_VR_PIN_HEAD=<degrees>: the eyes sit still at the reference head pose (+-32 mm), turned left by
    // that many degrees (any value, e.g. 1 or 0.0 for straight ahead): reproducible eye shots whatever the headset does
    static int pin = -1;
    static float pin_turn, pin_pitch;  // (",<degrees>" after the turn: also looking up by that many, negative: down)
    if (pin < 0) {
        pin = getenv("SKYRECOMP_VR_PIN_HEAD") != NULL;
        if (pin) pin_turn = (float)atof(getenv("SKYRECOMP_VR_PIN_HEAD")) * (float)(M_PI / 180.0);
        if (pin_turn == (float)(M_PI / 180.0)) pin_turn = 0;  // ("1": on)
        const char *comma = pin ? strchr(getenv("SKYRECOMP_VR_PIN_HEAD"), ',') : NULL;
        if (comma) pin_pitch = (float)atof(comma + 1) * (float)(M_PI / 180.0);
    }
    if (ok && pin) {
        static float spin = -1;  // (test) SKYRECOMP_VR_PIN_HEAD_SPIN=<degrees per second>: the pinned head keeps turning
        if (spin < 0) spin = getenv("SKYRECOMP_VR_PIN_HEAD_SPIN") ? (float)atof(getenv("SKYRECOMP_VR_PIN_HEAD_SPIN")) : 0;
        const float turn = pin_turn + spin * (float)(M_PI / 180.0) * (float)(SDL_GetTicksNS() / 1e9);
        const float ty = sinf(turn / 2), tw = cosf(turn / 2);  // reference (yaw only) * yaw(turn)
        const XrQuaternionf q = {0, reference_pose[1] * tw + reference_pose[3] * ty, 0, reference_pose[3] * tw - reference_pose[1] * ty};
        const float c = 1 - 2 * q.y * q.y, s = 2 * q.y * q.w;  // (the eyes' axis turns with the head)
        for (int e = 0; e < 2; e++) {
            const float x = e ? 0.032f : -0.032f;  // along the reference's +x = (c, 0, -s)
            const float px = sinf(pin_pitch / 2), pw = cosf(pin_pitch / 2);  // q * pitch(about the head's +x)
            frame.views[e].pose.orientation = (XrQuaternionf){q.w * px, q.y * pw, -q.y * px, q.w * pw};
            frame.views[e].pose.position = (XrVector3f){reference_pose[4] + x * c, reference_pose[5], reference_pose[6] - x * s};
        }
    }
    return ok;
}

bool vr_frame_begin(void) {
    if (!running) return false;
    if (frame.begun) return frame.stereo;
    static int wait_logged, begin_logged;
    set_stage(STAGE_WAIT_FRAME);
    XrFrameWaitInfo wi = {.type = XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
    const uint64_t wait_t0 = SDL_GetTicksNS();
    if (!xr_frame_ok(p_xrWaitFrame(xr_session, &wi, &fs), "xrWaitFrame", &wait_logged)) {
        set_stage(STAGE_IDLE);
        return false;
    }
    {   // The headset's nominal refresh (frame_rate.c): the shortest period it has predicted. SteamVR lengthens the
        // period to a fraction of the refresh while it throttles a late application, so the minimum is the real rate.
        static XrDuration shortest;
        extern int g_rt_headset_hz;
        if (fs.predictedDisplayPeriod > 1000000 && (!g_rt_headset_hz || fs.predictedDisplayPeriod < shortest)) {
            shortest = fs.predictedDisplayPeriod;
            g_rt_headset_hz = (int)(1e9 / (double)shortest + 0.5);
            LOG("[vr] headset refresh %d Hz\n", g_rt_headset_hz);
        }
    }
    {   // SKYRECOMP_VR_PACE_TRACE=1: every 300 frames, the headset's refresh and the time spent waiting for it
        static int trace = -1;
        static uint64_t waited, frames, last_ns;
        static uint64_t hist[4];  // frame intervals in headset periods: 1, 2, 3, more
        if (trace < 0) trace = getenv("SKYRECOMP_VR_PACE_TRACE") ? atoi(getenv("SKYRECOMP_VR_PACE_TRACE")) : 0;
        const uint64_t now = SDL_GetTicksNS();
        if (trace == 2) {  // each frame: predicted period, time waited, work since the previous wait returned
            static uint64_t prev_return, nlog;
            static XrTime prev_display;
            extern uint64_t vk_cum_wait_ns(void);
            static uint64_t prev_gpu_wait;
            const uint64_t gw = vk_cum_wait_ns();
            extern uint64_t nvn_async_idle_ns(void);
            static uint64_t prev_idle;
            const uint64_t idle = nvn_async_idle_ns();
            extern void vk_cum_lists(uint64_t *ns, uint64_t *lists, uint64_t *draws, uint64_t *passes);
            static uint64_t pl[4];
            uint64_t cl[4];
            vk_cum_lists(&cl[0], &cl[1], &cl[2], &cl[3]);
            if (nlog++ % 1000 < 120) {
                LOG("[vr] frame: lists %llu %.2f ms, %llu draws %llu passes | replay idle %.2f gpu waits %.2f period %.2f waited %.2f work %.2f display step %.2f ms\n",
                    (unsigned long long)(cl[1] - pl[1]), (cl[0] - pl[0]) / 1e6, (unsigned long long)(cl[2] - pl[2]),
                    (unsigned long long)(cl[3] - pl[3]), (idle - prev_idle) / 1e6, (gw - prev_gpu_wait) / 1e6, fs.predictedDisplayPeriod / 1e6,
                    (now - wait_t0) / 1e6, prev_return ? (wait_t0 - prev_return) / 1e6 : 0.0,
                    prev_display ? (double)(fs.predictedDisplayTime - prev_display) / 1e6 : 0.0);
            }
            prev_display = fs.predictedDisplayTime;
            prev_gpu_wait = gw;
            prev_idle = idle;
            memcpy(pl, cl, sizeof pl);
            prev_return = now;
        }
        { extern uint64_t g_vr_wait_ns; g_vr_wait_ns += now - wait_t0; }  // (exec.c profile, LARECOMP_VK_PROF)
        { extern __thread uint64_t g_blocked_ns; g_blocked_ns += now - wait_t0; }  // (pacing stats: waiting, not work)
        if (trace) {
            waited += now - wait_t0;
            if (last_ns && fs.predictedDisplayPeriod > 0) {
                const double periods = (double)(now - last_ns) / (double)fs.predictedDisplayPeriod;
                hist[periods < 1.5 ? 0 : periods < 2.5 ? 1 : periods < 3.5 ? 2 : 3]++;
            }
            last_ns = now;
            if (++frames == 300) {
                LOG("[vr] pace: headset %.1f Hz, xrWaitFrame %.2f ms/frame, intervals 1:%llu 2:%llu 3:%llu more:%llu\n",
                    fs.predictedDisplayPeriod > 0 ? 1e9 / (double)fs.predictedDisplayPeriod : 0.0, waited / 1e6 / frames,
                    (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                    (unsigned long long)hist[3]);
                LOG("[vr] pace: per frame ms: idle %.2f wait %.2f begin %.2f render %.2f acquire %.2f wait-image %.2f end %.2f, GPU (last) %.2f\n",
                    stage_ns[0] / 1e6 / frames, stage_ns[1] / 1e6 / frames, stage_ns[2] / 1e6 / frames,
                    stage_ns[3] / 1e6 / frames, stage_ns[4] / 1e6 / frames, stage_ns[5] / 1e6 / frames,
                    stage_ns[6] / 1e6 / frames, g_vr_gpu_ms);
                memset(stage_ns, 0, sizeof stage_ns);
                waited = frames = 0;
                memset(hist, 0, sizeof hist);
            }
        }
    }
    set_stage(STAGE_BEGIN_FRAME);
    XrFrameBeginInfo bi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_frame_ok(p_xrBeginFrame(xr_session, &bi), "xrBeginFrame", &begin_logged)) {
        set_stage(STAGE_IDLE);
        return false;
    }
    frame.begun = true;
    frame.display_time = fs.predictedDisplayTime;
    frame.display_period = fs.predictedDisplayPeriod;
    {   pthread_mutex_lock(&timing_lock);
        const int64_t timing_wall = (int64_t)SDL_GetTicksNS();
        // (the display lead, averaged: the render thread's begin times wobble by up to a frame - two begins close
        // together, then a gap - and following each one made the menu pointer step big, small, big)
        const double lead = (double)(fs.predictedDisplayTime - timing_wall);
        timing_lead = timing_lead_ok && fabs(lead - timing_lead) < 1e8 ? timing_lead + (lead - timing_lead) * 0.02 : lead;
        timing_lead_ok = true;
        pthread_mutex_unlock(&timing_lock);
    }
    frame.work_start_ns = SDL_GetTicksNS();
    frame.stereo = fs.shouldRender && locate_views();
    static bool hidden_logged;
    if (frame.stereo && !hidden_logged && (hidden_n[0] || hidden_n[1])) {  // (how much of each eye image is hidden)
        hidden_logged = true;
        for (int e = 0; e < 2; e++) {
            const XrFovf *f = &frame.views[e].fov;
            const double fw = tan(f->angleRight) - tan(f->angleLeft), fh = tan(f->angleUp) - tan(f->angleDown);
            double area = 0;
            for (uint32_t i = 0; i + 2 < hidden_n[e]; i += 3) {
                const float *a = hidden_xy[e] + i * 2, *b = a + 2, *c = a + 4;
                area += fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) * 0.5;
            }
            LOG("[vr] eye %d: the headset hides %.1f%% of the eye image\n", e, 100.0 * area / (fw * fh));
        }
    }
    vr_rig_begin_frame();
    if (frame.stereo) update_cull();
    set_stage(STAGE_RENDER);
    return frame.stereo;
}

bool vr_stereo_active(void) { return frame.begun && frame.stereo; }

void vr_recenter(void) {
    reference_valid = false;  // (the next tracked head pose is the new reference)
    vr_rig_recenter();
    ResetHUDLazyFollow();  // (the HUD panel: in front of the head again)
}


// This frame's eyes for the rig's culling field of view (vr_rig_cull_fov, runtime/hle/widescreen.c).
static void update_cull(void) {
    float eye[2][7], fov[2][4];
    for (int e = 0; e < 2; e++) {
        store_pose(eye[e], &frame.views[e].pose);
        const XrFovf *f = &frame.views[e].fov;
        fov[e][0] = f->angleLeft; fov[e][1] = f->angleRight; fov[e][2] = f->angleUp; fov[e][3] = f->angleDown;
    }
    vr_rig_update_cull(reference_pose, eye[0], fov[0], eye[1], fov[1]);
    vr_rig_set_near_clip(g_settings.vr_near_clip);  // (VR Options > Near Clip)
}

static bool patch_scene_ubo(uint32_t eye, float out[28], const float *in, uint64_t size, bool fragment) {
    if (eye > 1 || !out || !in || size < SKY_VR_CAMERA_FLOATS * sizeof(float) || !vr_stereo_active()) return false;
    static int off = -1;  // (test) SKYRECOMP_VR_NO_CAMERA=1: both eyes keep the game's camera
    if (off < 0) off = getenv("SKYRECOMP_VR_NO_CAMERA") != NULL;
    if (off) return false;
    float pose[7];
    store_pose(pose, &frame.views[eye].pose);
    const XrFovf *f = &frame.views[eye].fov;
    const float fov[4] = {f->angleLeft, f->angleRight, f->angleUp, f->angleDown};
    bool ok = fragment ? vr_rig_eye_camera_direct(in, reference_pose, pose, fov, out)
                       : vr_rig_eye_camera(in, reference_pose, pose, fov, out);
    frame.patched[eye] += ok;
    static int dumped;  // (trace) the first few blocks rejected as a scene camera, to check the ABI against the game
    if (!ok && trace_enabled() && dumped < 6) {
        dumped++;
        char line[640];
        int n = 0;
        for (int i = 0; i < SKY_VR_CAMERA_FLOATS && n < (int)sizeof line - 16; i++) n += snprintf(line + n, sizeof line - n, " %g", in[i]);
        LOG("[vr] slot 0 block not taken as the scene camera:%s\n", line);
    }
    return ok;
}

bool vr_patch_scene_ubo(uint32_t eye, float out[28], const float *in, uint64_t size) {
    return patch_scene_ubo(eye, out, in, size, false);
}

bool vr_patch_scene_fragment_ubo(uint32_t eye, float out[28], const float *in, uint64_t size) {
    return patch_scene_ubo(eye, out, in, size, true);
}

static void eye_barrier_layers(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                               VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, uint32_t layers) {
    VkPipelineStageFlags2 src_stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 src_access = 0;
    if (from == VK_IMAGE_LAYOUT_GENERAL) {
        src_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        src_access = VK_ACCESS_2_MEMORY_WRITE_BIT;
    } else if (from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else if (from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        src_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    } else if (from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    // Synchronize only this image. The old global memory barrier stalled every
    // unrelated render target, descriptor read and upload before each XR copy.
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = src_stage, .srcAccessMask = src_access,
                                .dstStageMask = dst_stage, .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    eye_barrier_layers(cb, image, from, to, dst_stage, dst_access, 2);
}

// Frozen world: full-screen 2D screens (the map) draw no 3D scene. The eyes then show the last frame that had one,
// submitted with the poses it was rendered for, so the compositor keeps it fixed in the room as the head moves.
static VkImage frozen;                  // the swapchain's format, 2 layers: the last 3D frame's eye images
static VkDeviceMemory frozen_memory;
static XrView frozen_views[2];
static bool frozen_valid, frozen_failed, freeze_next, frame_3d_next;

void vr_freeze_world(bool freeze, bool frame_is_3d) { freeze_next = freeze; frame_3d_next = frame_is_3d; }
// A prerendered cutscene (device.c): the eyes dark, the HUD panel (the video) larger and held still - a cinema.
static bool g_cinema;
void vr_cinema(bool on) { g_cinema = on; }
static bool g_black_background;
void vr_black_background(bool on) { g_black_background = on; }
static float hud_size_factor(void) { return g_cinema ? 1.6f : 1.0f; }

static void frozen_destroy(void) {
    if (vk_device && frozen) vkDestroyImage(vk_device, frozen, NULL);
    if (vk_device && frozen_memory) vkFreeMemory(vk_device, frozen_memory, NULL);
    frozen = VK_NULL_HANDLE;
    frozen_memory = VK_NULL_HANDLE;
    frozen_valid = frozen_failed = false;
}

static bool frozen_create(void) {
    if (frozen) return true;
    if (frozen_failed || swapchain_format == VK_FORMAT_UNDEFINED) return false;
    frozen_failed = true;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = swapchain_format,
                            .extent = {eye_width, eye_height, 1}, .mipLevels = 1, .arrayLayers = 2,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(vk_device, &ci, NULL, &frozen) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_device, frozen, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(vk_device, &ai, NULL, &frozen_memory) != VK_SUCCESS ||
        vkBindImageMemory(vk_device, frozen, frozen_memory, 0) != VK_SUCCESS)
        return false;
    frozen_failed = false;
    return true;
}

// (device.c, each present) The part of the presented image the game shows, as fractions of it (origin, size): its
// dynamic resolution draws a smaller frame into a corner, its window crop says which (the desktop applies it too).
static float g_eye_crop[4] = {0, 0, 1, 1};
void vr_set_eye_crop(const float crop[4]) {
    const bool ok = crop && crop[2] > 0.05f && crop[3] > 0.05f && crop[0] >= 0 && crop[1] >= 0 && crop[0] + crop[2] <= 1.001f &&
                    crop[1] + crop[3] <= 1.001f;
    const float full[4] = {0, 0, 1, 1};
    memcpy(g_eye_crop, ok ? crop : full, sizeof g_eye_crop);
}

bool vr_record_eye_copies(void *command_buffer, void *left_image, void *right_image, uint32_t sw, uint32_t sh, bool flip_y,
                          uint32_t source_format, uint32_t left_layer, uint32_t right_layer) {
    if (!vr_stereo_active() || frame.acquired || !command_buffer || !left_image || !sw || !sh) return false;
    if (!right_image) {
        static bool logged;
        if (!logged) LOG("[vr] presented image has no eye-1 copy (not rendered this frame); showing it to both eyes\n");
        logged = true;
        right_image = left_image;
        right_layer = left_layer;
    }
    static int acquire_logged, wait_logged;
    set_stage(STAGE_ACQUIRE);
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xr_frame_ok(p_xrAcquireSwapchainImage(swapchain, &ai, &frame.image), "xrAcquireSwapchainImage", &acquire_logged) ||
        frame.image >= image_count) {
        set_stage(STAGE_RENDER);
        return false;
    }
    // Wait until the compositor is done with the image. XR_TIMEOUT_EXPIRED is a success code: wait again.
    set_stage(STAGE_WAIT_IMAGE);
    XrSwapchainImageWaitInfo wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000ll};
    XrResult r = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; r == XR_TIMEOUT_EXPIRED && tries < 20; tries++) r = p_xrWaitSwapchainImage(swapchain, &wait);
    set_stage(STAGE_RENDER);
    if (r != XR_SUCCESS) {
        // An acquired image that was never waited can't be released; it is lost to the swapchain. Rare (compositor
        // hang), and the next acquire reports it.
        if (wait_logged++ < 5) LOG("[vr] xrWaitSwapchainImage %s (%d)\n", r == XR_TIMEOUT_EXPIRED ? "timed out" : "failed", (int)r);
        return false;
    }
    frame.acquired = true;

    VkCommandBuffer cb = command_buffer;
    { extern void vr_marker_fill(VkCommandBuffer cb); vr_marker_fill(cb); }  // (the Slingshot's marker image, once)
    VkImage dst = images[frame.image].image;
    const bool freeze = freeze_next, frame_3d = frame_3d_next;
    freeze_next = frame_3d_next = false;
    if (g_cinema || g_black_background) {  // Video and black title cards replace the world behind their lettering.
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue black = {{0, 0, 0, 1}};
        const VkImageSubresourceRange both = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &both);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        frame.copied = true;
        return true;
    }
    if (freeze && frozen_valid) {  // (vr_freeze_world: the last 3D frame again, with its poses)
        barrier(cb, frozen, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2}, .extent = {eye_width, eye_height, 1}};
        vkCmdCopyImage(cb, frozen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, frozen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        frame.frozen_used = true;
        frame.copied = true;
        return true;
    }
    VkImage src[2] = {left_image, right_image};
    // Colour: the game's image holds display-ready (gamma-encoded) values; the desktop presents them unchanged. A blit
    // into the sRGB swapchain from a UNORM image would encode them a second time (washed-out colours), so when the
    // transfer functions differ the eyes go through a UNORM staging image and are copied into the swapchain as raw
    // bytes, which the compositor then decodes as sRGB once.
    bool needs_swizzle = false, dst_srgb = false;
    const bool direct = ValidateVulkanToOpenXRFormat(source_format, (uint32_t)swapchain_format, &needs_swizzle, &dst_srgb) ||
                        !staging;
    static uint32_t logged_format = ~0u;
    if (logged_format != source_format) {
        logged_format = source_format;
        LOG("[vr] eye copy: game image format %u -> swapchain format %d (%s): %s\n", source_format, swapchain_format,
            dst_srgb ? "sRGB" : "linear", direct ? "blit" : "raw copy through a staging image (no second gamma encoding)");
    }
    VkImage blit_dst = direct ? dst : staging;
    // (every layer: an eye may be layer 1 of an image holding both - textures.c eye layers)
    eye_barrier_layers(cb, src[0], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_REMAINING_ARRAY_LAYERS);
    if (src[1] != src[0])
        eye_barrier_layers(cb, src[1], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_REMAINING_ARRAY_LAYERS);
    if (!direct) barrier(cb, staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // The game image fills the eye image whole: its projection already maps the headset's frustum to [-1, 1], so
    // the stretch restores the correct shape whatever the render target's aspect.
    // (vr_set_eye_crop: the part of the image the game shows - its dynamic resolution renders into a corner)
    const int32_t cx0 = (int32_t)lroundf(g_eye_crop[0] * sw), cx1 = (int32_t)lroundf((g_eye_crop[0] + g_eye_crop[2]) * sw);
    const int32_t cy0 = (int32_t)lroundf(g_eye_crop[1] * sh), cy1 = (int32_t)lroundf((g_eye_crop[1] + g_eye_crop[3]) * sh);
    int32_t y0 = cy0, y1 = cy1;
    if (flip_y) { y0 = cy1; y1 = cy0; }
    for (uint32_t eye = 0; eye < 2; eye++) {
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye ? right_layer : left_layer, 1},
                            .srcOffsets = {{cx0, y0, 0}, {cx1, y1, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1},
                            .dstOffsets = {{0, 0, 0}, {(int32_t)eye_width, (int32_t)eye_height, 1}}};
        vkCmdBlitImage(cb, src[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    eye_barrier_layers(cb, src[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_REMAINING_ARRAY_LAYERS);
    if (src[1] != src[0])
        eye_barrier_layers(cb, src[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_REMAINING_ARRAY_LAYERS);
    if (!direct) {
        barrier(cb, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2},
                                  .extent = {eye_width, eye_height, 1}};
        vkCmdCopyImage(cb, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    // The frozen world (vr_freeze_world): this 3D frame's eyes, kept with their poses. (Not a frame that is itself
    // full-screen 2D: its eye images hold the flat screen.)
    if (!freeze && frame_3d && vk_hud_layer_enabled() && frozen_create()) {
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, frozen, frozen_valid ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2}, .extent = {eye_width, eye_height, 1}};
        vkCmdCopyImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frozen, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        frozen_views[0] = frame.views[0];
        frozen_views[1] = frame.views[1];
        frozen_valid = true;
    }
    // The runtime takes the image back in COLOR_ATTACHMENT_OPTIMAL (swapchain created with the color attachment usage).
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    frame.copied = true;
    return true;
}

// ================================================================== HUD panel (the game's 2D layer on a quad)
// exec.c draws the 2D layer into a 1920x1080 texture of its own (SKYRECOMP_VR_HUD_LAYER); it goes to an OpenXR quad
// layer about 2 m in front of the player (runtime/vr_hud.h: distance, scale, smooth follow of the head's yaw).
enum { HUD_W = 1920, HUD_H = 1080 };
static XrSwapchain hud_swapchain;
static XrSwapchainImageVulkanKHR *hud_images;
static uint32_t hud_count;
static VkImage hud_staging;  // UNORM, as the eyes' staging image: raw bytes into the sRGB swapchain
static VkDeviceMemory hud_staging_memory;
static VkImageView hud_staging_view;
static VkPipeline hud_backing_pipeline;
static VkPipelineLayout hud_backing_layout;
static bool hud_failed;

static void hud_destroy(void) {
    if (vk_device && hud_backing_pipeline) vkDestroyPipeline(vk_device, hud_backing_pipeline, NULL);
    if (vk_device && hud_backing_layout) vkDestroyPipelineLayout(vk_device, hud_backing_layout, NULL);
    if (vk_device && hud_staging_view) vkDestroyImageView(vk_device, hud_staging_view, NULL);
    if (vk_device && hud_staging) vkDestroyImage(vk_device, hud_staging, NULL);
    if (vk_device && hud_staging_memory) vkFreeMemory(vk_device, hud_staging_memory, NULL);
    hud_staging = VK_NULL_HANDLE;
    hud_staging_memory = VK_NULL_HANDLE;
    hud_staging_view = VK_NULL_HANDLE;
    hud_backing_pipeline = VK_NULL_HANDLE;
    hud_backing_layout = VK_NULL_HANDLE;
    SDL_free(hud_images);
    hud_images = NULL;
    hud_count = 0;
    if (hud_swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(hud_swapchain);
    hud_swapchain = XR_NULL_HANDLE;
    hud_failed = false;
}

static bool hud_backing_create(VkFormat format) {
    static const char *vs_source =
        "#version 450\n"
        "void main(){ vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); gl_Position=vec4(p[gl_VertexIndex],0,1); }\n";
    static const char *fs_source =
        "#version 450\n"
        "layout(push_constant) uniform P { vec4 color; } pc; layout(location=0) out vec4 out_color;\n"
        "void main(){ out_color=pc.color; }\n";
    size_t vn = 0, fn = 0;
    uint32_t *vspv = vk_glsl_to_spirv(vs_source, STAGE_VERTEX, "vr wrist backing vertex", &vn);
    uint32_t *fspv = vk_glsl_to_spirv(fs_source, STAGE_FRAGMENT, "vr wrist backing fragment", &fn);
    if (!vspv || !fspv) { free(vspv); free(fspv); return false; }
    VkShaderModule vm = vk_module(vspv, vn), fm = vk_module(fspv, fn);
    free(vspv); free(fspv);
    if (!vm || !fm) return false;
    const VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float)};
    const VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                           .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (vkCreatePipelineLayout(vk_device, &li, NULL, &hud_backing_layout) != VK_SUCCESS) return false;
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}};
    const VkPipelineVertexInputStateCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo ia = {.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo vp = {.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                   .viewportCount=1,.scissorCount=1};
    const VkPipelineRasterizationStateCreateInfo rs = {.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    const VkPipelineMultisampleStateCreateInfo ms = {.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                      .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState ba = {.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,.dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .colorBlendOp=VK_BLEND_OP_ADD,.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE,.alphaBlendOp=VK_BLEND_OP_ADD,
        .colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo bs = {.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                     .attachmentCount=1,.pAttachments=&ba};
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo ds = {.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                  .dynamicStateCount=2,.pDynamicStates=dynamics};
    const VkPipelineRenderingCreateInfo rendering = {.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                      .colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    const VkGraphicsPipelineCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,
        .pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&bs,.pDynamicState=&ds,.layout=hud_backing_layout};
    const VkResult result = vkCreateGraphicsPipelines(vk_device, VK_NULL_HANDLE, 1, &pi, NULL, &hud_backing_pipeline);
    vkDestroyShaderModule(vk_device, vm, NULL); vkDestroyShaderModule(vk_device, fm, NULL);
    return result == VK_SUCCESS;
}

static bool hud_create(void) {
    if (hud_swapchain) return true;
    if (hud_failed || !xr_session || swapchain_format == VK_FORMAT_UNDEFINED) return false;
    hud_failed = true;  // (until it all succeeds)
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | MIRROR_USAGE,
                                .format = swapchain_format, .sampleCount = 1, .width = HUD_W, .height = HUD_H,
                                .faceCount = 1, .arraySize = 1, .mipCount = 1};
    if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &hud_swapchain), "xrCreateSwapchain (HUD)") ||
        !xr_ok(p_xrEnumerateSwapchainImages(hud_swapchain, 0, &hud_count, NULL), "xrEnumerateSwapchainImages (HUD)") ||
        !hud_count)
        return false;
    hud_images = SDL_calloc(hud_count, sizeof *hud_images);
    if (!hud_images) return false;
    for (uint32_t i = 0; i < hud_count; i++) hud_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!xr_ok(p_xrEnumerateSwapchainImages(hud_swapchain, hud_count, &hud_count, (XrSwapchainImageBaseHeader *)hud_images),
               "xrEnumerateSwapchainImages (HUD)"))
        return false;
    const VkFormat f = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                     : swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
    if (f != VK_FORMAT_UNDEFINED) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {HUD_W, HUD_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(vk_device, &ii, NULL, &hud_staging) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk_device, hud_staging, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mp);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || vkAllocateMemory(vk_device, &ai, NULL, &hud_staging_memory) != VK_SUCCESS ||
            vkBindImageMemory(vk_device, hud_staging, hud_staging_memory, 0) != VK_SUCCESS)
            return false;
        const VkImageViewCreateInfo view = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=hud_staging,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=f,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if (vkCreateImageView(vk_device, &view, NULL, &hud_staging_view) != VK_SUCCESS || !hud_backing_create(f)) return false;
    }
    hud_failed = false;
    LOG("[vr] HUD panel swapchain: %ux%u, %u images, format %d\n", HUD_W, HUD_H, hud_count, swapchain_format);
    return true;
}

static void barrier_layers(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access, uint32_t layers) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = dst_stage, .dstAccessMask = dst_access};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .dstStageMask = dst_stage, .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

// A final head-locked overlay covers both eyes, including all earlier HUD/menu overlays.
static void view_fade_destroy(void) {
    if (view_fade_swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(view_fade_swapchain);
    view_fade_swapchain = XR_NULL_HANDLE;
    SDL_free(view_fade_images);
    view_fade_images = NULL;
    view_fade_count = 0;
    view_fade_acquired = view_fade_ready = view_fade_failed = false;
}

bool vr_view_fade_available(void) {
    const char *enabled = ENV("SKYRECOMP_VR_FULL_VIEW_FADE");
    if (enabled && !atoi(enabled)) return false;
    if (!vr_stereo_active() || view_fade_failed) return false;
    if (!view_fade_swapchain) {
        view_fade_failed = true;
        XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | MIRROR_USAGE,
            .format = swapchain_format, .sampleCount = 1, .width = 8, .height = 8,
            .faceCount = 1, .arraySize = 1, .mipCount = 1};
        if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &view_fade_swapchain), "xrCreateSwapchain (fade)") ||
            !xr_ok(p_xrEnumerateSwapchainImages(view_fade_swapchain, 0, &view_fade_count, NULL), "xrEnumerateSwapchainImages (fade)") || !view_fade_count)
            return false;
        view_fade_images = SDL_calloc(view_fade_count, sizeof *view_fade_images);
        if (!view_fade_images) return false;
        for (uint32_t i = 0; i < view_fade_count; i++) view_fade_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        if (!xr_ok(p_xrEnumerateSwapchainImages(view_fade_swapchain, view_fade_count, &view_fade_count,
                    (XrSwapchainImageBaseHeader *)view_fade_images), "xrEnumerateSwapchainImages (fade)")) return false;
        view_fade_failed = false;
    }
    return true;
}

bool vr_record_view_fade(void *command_buffer, const float rgba[4]) {
    if (!vr_view_fade_available() || !command_buffer || rgba[3] <= 0 || view_fade_acquired) return false;
    if (!xr_ok(p_xrAcquireSwapchainImage(view_fade_swapchain,
                &(XrSwapchainImageAcquireInfo){.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}, &view_fade_index), "xrAcquireSwapchainImage (fade)")) return false;
    view_fade_acquired = true;
    XrSwapchainImageWaitInfo wi = {.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout=100000000ll};
    XrResult result = XR_TIMEOUT_EXPIRED;
    for (int i = 0; result == XR_TIMEOUT_EXPIRED && i < 20; i++) result = p_xrWaitSwapchainImage(view_fade_swapchain, &wi);
    if (result != XR_SUCCESS || view_fade_index >= view_fade_count) return false;
    VkCommandBuffer cb = command_buffer;
    VkImage image = view_fade_images[view_fade_index].image;
    barrier_layers(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    float linear[4];
    vr_fade_compositor_color(rgba,linear);
    const VkClearColorValue color = {{linear[0], linear[1], linear[2], linear[3]}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier_layers(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 1);
    view_fade_ready = true;
    if (getenv("SKYRECOMP_VR_FADE_TRACE")) {
        static unsigned traced;
        if (traced++ % 30 == 0)
            LOG("[vr-fade] native opacity %.6f -> compositor linear opacity %.6f\n",rgba[3],linear[3]);
    }
    return true;
}

bool vr_record_hud_copy(void *command_buffer, void *hud_image, uint32_t w, uint32_t h, bool flip_y, bool opaque) {
    if (!vr_stereo_active() || frame.hud_acquired || !command_buffer || !hud_image || !w || !h || !hud_create()) return false;
    static int acquire_logged, wait_logged;
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xr_frame_ok(p_xrAcquireSwapchainImage(hud_swapchain, &ai, &frame.hud_image), "xrAcquireSwapchainImage (HUD)",
                     &acquire_logged) || frame.hud_image >= hud_count)
        return false;
    XrSwapchainImageWaitInfo wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000ll};
    XrResult r = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; r == XR_TIMEOUT_EXPIRED && tries < 20; tries++) r = p_xrWaitSwapchainImage(hud_swapchain, &wait);
    if (r != XR_SUCCESS) {
        if (wait_logged++ < 5) LOG("[vr] xrWaitSwapchainImage (HUD) failed (%d)\n", (int)r);
        return false;
    }
    frame.hud_acquired = true;
    VkCommandBuffer cb = command_buffer;
    VkImage dst = hud_images[frame.hud_image].image;
    VkImage blit_dst = hud_staging ? hud_staging : dst;  // (as the eyes: no second sRGB encoding)
    if (hud_staging) barrier_layers(cb, hud_staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    barrier_layers(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    extern bool player_eye(float eye[3]);
    const bool compose_wrist = !opaque && g_vr_hud_config.wrist_hud && player_eye(NULL) &&
                               (hud_grip_tracked[0] || hud_grip_tracked[1]) && w == HUD_W && h == HUD_H;
    if (!compose_wrist) {
        int32_t y0 = 0, y1 = (int32_t)h;
        if (flip_y) { y0 = (int32_t)h; y1 = 0; }
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, y0, 0}, {(int32_t)w, y1, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {HUD_W, HUD_H, 1}}};
        vkCmdBlitImage(cb, hud_image, VK_IMAGE_LAYOUT_GENERAL, blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    } else {
        // Build two real 480x480 atlases in the OpenXR image. Each atlas is later displayed by one quad; the
        // individual HUD elements are pixels in that image, not additional composition layers.
        // Premultiplied dark backing, similar to Shipwright's wrist panel. The centre area is immediately replaced
        // with the original transparent HUD, so this backing exists only inside the two sampled wrist squares.
        const VkClearColorValue backing = {{0, 0, 0, 0}};
        const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &backing, 1, &all);
        typedef struct { int sx, sy, sw, sh, dx0, dy0, dx1, dy1; } AtlasBlit;
        const AtlasBlit parts[] = {
            {540, 0, 900, 1080, 540, 0, 1440, 1080}, // centred prompts, clear of the wide heart row at the left edge
            // Left wrist, matching ss-module template.png: hearts on top, shield gauge and rupees in the
            // middle, D-pad/help at the bottom. All destinations lie in the same 480x480 atlas.
            {40, 940, 470, 110, 10, 8, 470, 125},    // hearts only; begins above the shield module's glow
            {45, 850, 205, 90, 10, 135, 190, 220},  // shield icon and gauge, clear of the centre hand silhouette
            {45, 45, 185, 65, 265, 150, 465, 220},  // rupees only; stops before the lower status icon begins
            {35, 105, 195, 220, 125, 245, 355, 440},// D-pad/help/status, including the complete lower icon and glow
            // Right wrist: item slot above pouch, as marked in the supplied template.
            {1715, 805, 170, 235, 1550, 15, 1810, 255}, // complete item slot and its single label
            {1735, 660, 145, 155, 1555, 280, 1805, 465} // tightly isolated pouch module
        };
        VkImageBlit blits[sizeof parts / sizeof *parts];
        for (uint32_t i = 0; i < sizeof parts / sizeof *parts; i++) {
            const AtlasBlit *p = &parts[i];
            const int sy0 = flip_y ? p->sy + p->sh : p->sy;
            const int sy1 = flip_y ? p->sy : p->sy + p->sh;
            blits[i] = (VkImageBlit){
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .srcOffsets = {{p->sx, sy0, 0}, {p->sx + p->sw, sy1, 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .dstOffsets = {{p->dx0, p->dy0, 0}, {p->dx1, p->dy1, 1}}};
        }
        vkCmdBlitImage(cb, hud_image, VK_IMAGE_LAYOUT_GENERAL, blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       (uint32_t)(sizeof blits / sizeof *blits), blits, VK_FILTER_LINEAR);
    }
    VkImageLayout staging_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (compose_wrist && hud_staging && hud_staging_view && hud_backing_pipeline) {
        barrier_layers(cb, hud_staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1);
        const VkRenderingAttachmentInfo attachment = {.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView=hud_staging_view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo rendering = {.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea={{0,0},{HUD_W,HUD_H}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&attachment};
        vkCmdBeginRendering(cb, &rendering);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hud_backing_pipeline);
        const VkViewport viewport = {0, 0, HUD_W, HUD_H, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        const float alpha = g_vr_hud_config.wrist_hud_background;
        const float color[4] = {.022f*alpha, .026f*alpha, .039f*alpha, alpha};
        vkCmdPushConstants(cb, hud_backing_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof color, color);
        const VkRect2D panels[2] = {{{0,0},{480,480}},{{1440,0},{480,480}}};
        for (int i = 0; i < 2; i++) { vkCmdSetScissor(cb, 0, 1, &panels[i]); vkCmdDraw(cb, 3, 1, 0, 0); }
        vkCmdEndRendering(cb);
        staging_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (hud_staging) {
        barrier_layers(cb, hud_staging, staging_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 1);
        const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {HUD_W, HUD_H, 1}};
        vkCmdCopyImage(cb, hud_staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    barrier_layers(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 1);
    frame.hud_copied = true;
    frame.hud_opaque = opaque;
    return true;
}

// ================================================================== the settings menu's panel and its beam
// (After ShipWright VR.) The settings menu / name entry (ui/menu.mm) in its own 1600 x 1000 quad layer: placed level
// in front of the gaze when it opens (g_vr_hud_config.menu_*), then still in the world. The controller's ray aims at
// it (vr_controls.c); a beam quad runs from the pointing controller to where it hits, fading out toward the far end.
enum { MENU_W = 1600, MENU_H = 1000, BEAM_W = 4, BEAM_H = 128 };
static XrSwapchain menu_swapchain, beam_swapchain;
static XrSwapchainImageVulkanKHR *menu_images, beam_image;
static uint32_t menu_count;
static VkImage menu_staging;
static VkDeviceMemory menu_staging_memory;
static VkBuffer beam_buffer;
static VkDeviceMemory beam_buffer_memory;
static bool menu_failed, beam_failed, beam_ready;
static bool menu_acquired, menu_copied;
static uint32_t menu_image_index;
static float g_menu_world[16];  // world-from-panel: columns right, up, back (unit), centre
static bool g_menu_placed, g_menu_was_passive;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void menu_destroy(void) {
    if (vk_device) {
        if (menu_staging) vkDestroyImage(vk_device, menu_staging, NULL);
        if (menu_staging_memory) vkFreeMemory(vk_device, menu_staging_memory, NULL);
        if (beam_buffer) vkDestroyBuffer(vk_device, beam_buffer, NULL);
        if (beam_buffer_memory) vkFreeMemory(vk_device, beam_buffer_memory, NULL);
    }
    menu_staging = VK_NULL_HANDLE, menu_staging_memory = VK_NULL_HANDLE;
    beam_buffer = VK_NULL_HANDLE, beam_buffer_memory = VK_NULL_HANDLE;
    SDL_free(menu_images);
    menu_images = NULL;
    menu_count = 0;
    if (menu_swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(menu_swapchain);
    if (beam_swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(beam_swapchain);
    menu_swapchain = beam_swapchain = XR_NULL_HANDLE;
    menu_failed = beam_failed = beam_ready = false;
    {   // (the Slingshot's marker, with them)
        extern void vr_marker_destroy(void);
        vr_marker_destroy();
    }
    menu_acquired = menu_copied = false;
    g_menu_placed = false;
}

static bool menu_create(void) {
    if (menu_swapchain) return true;
    if (menu_failed || !xr_session || swapchain_format == VK_FORMAT_UNDEFINED) return false;
    menu_failed = true;  // (until it all succeeds)
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | MIRROR_USAGE,
                                .format = swapchain_format, .sampleCount = 1, .width = MENU_W, .height = MENU_H,
                                .faceCount = 1, .arraySize = 1, .mipCount = 1};
    if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &menu_swapchain), "xrCreateSwapchain (menu)") ||
        !xr_ok(p_xrEnumerateSwapchainImages(menu_swapchain, 0, &menu_count, NULL), "xrEnumerateSwapchainImages (menu)") ||
        !menu_count)
        return false;
    menu_images = SDL_calloc(menu_count, sizeof *menu_images);
    if (!menu_images) return false;
    for (uint32_t i = 0; i < menu_count; i++) menu_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!xr_ok(p_xrEnumerateSwapchainImages(menu_swapchain, menu_count, &menu_count, (XrSwapchainImageBaseHeader *)menu_images),
               "xrEnumerateSwapchainImages (menu)"))
        return false;
    // (as the eyes and the HUD: display-ready bytes into the sRGB swapchain through a UNORM image, no second encoding)
    const VkFormat f = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                     : swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
    if (f != VK_FORMAT_UNDEFINED) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {MENU_W, MENU_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(vk_device, &ii, NULL, &menu_staging) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk_device, menu_staging, &mr);
        const uint32_t type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || vkAllocateMemory(vk_device, &ai, NULL, &menu_staging_memory) != VK_SUCCESS ||
            vkBindImageMemory(vk_device, menu_staging, menu_staging_memory, 0) != VK_SUCCESS)
            return false;
    }
    menu_failed = false;
    LOG("[vr] menu panel swapchain: %ux%u, %u images\n", MENU_W, MENU_H, menu_count);
    return true;
}

// The beam's strip, once: a static image, transparent at the top (the far end) and fading in toward the hand.
static void beam_fill(VkCommandBuffer cb) {
    if (beam_ready || beam_failed || !xr_session || swapchain_format == VK_FORMAT_UNDEFINED) return;
    beam_failed = true;  // (until it all succeeds)
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT,
                                .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | MIRROR_USAGE,
                                .format = swapchain_format, .sampleCount = 1, .width = BEAM_W, .height = BEAM_H,
                                .faceCount = 1, .arraySize = 1, .mipCount = 1};
    uint32_t n = 1;
    beam_image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &beam_swapchain), "xrCreateSwapchain (beam)") ||
        !xr_ok(p_xrEnumerateSwapchainImages(beam_swapchain, 1, &n, (XrSwapchainImageBaseHeader *)&beam_image),
               "xrEnumerateSwapchainImages (beam)"))
        return;
    const VkDeviceSize size = BEAM_W * BEAM_H * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk_device, &bi, NULL, &beam_buffer) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_device, beam_buffer, &mr);
    const uint32_t type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    uint8_t *px;
    if (type == UINT32_MAX || vkAllocateMemory(vk_device, &ai, NULL, &beam_buffer_memory) != VK_SUCCESS ||
        vkBindBufferMemory(vk_device, beam_buffer, beam_buffer_memory, 0) != VK_SUCCESS ||
        vkMapMemory(vk_device, beam_buffer_memory, 0, size, 0, (void **)&px) != VK_SUCCESS)
        return;
    const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format == VK_FORMAT_B8G8R8A8_UNORM;
    for (int y = 0; y < BEAM_H; y++) {
        const float a = 0.85f * (float)(y + 1) / BEAM_H;  // (premultiplied light blue)
        const uint8_t r = (uint8_t)(0.55f * a * 255), g = (uint8_t)(0.85f * a * 255), b = (uint8_t)(a * 255), al = (uint8_t)(a * 255);
        for (int x = 0; x < BEAM_W; x++) {
            uint8_t *p = px + (y * BEAM_W + x) * 4;
            p[0] = bgra ? b : r, p[1] = g, p[2] = bgra ? r : b, p[3] = al;
        }
    }
    vkUnmapMemory(vk_device, beam_buffer_memory);
    uint32_t index;
    if (!xr_ok(p_xrAcquireSwapchainImage(beam_swapchain, &(XrSwapchainImageAcquireInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}, &index),
               "xrAcquireSwapchainImage (beam)") ||
        !xr_ok(p_xrWaitSwapchainImage(beam_swapchain, &(XrSwapchainImageWaitInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                                                                                    .timeout = XR_INFINITE_DURATION}),
               "xrWaitSwapchainImage (beam)"))
        return;
    barrier_layers(cb, beam_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {BEAM_W, BEAM_H, 1}};
    vkCmdCopyBufferToImage(cb, beam_buffer, beam_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier_layers(cb, beam_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 1);
    // (released at this frame's end, after the copy is recorded: the runtime waits for the submission)
    beam_failed = false;
    beam_ready = true;
}

// ================================================================== the Slingshot's marker
// Where its seed will land (runtime/hle/vr_aim.c vr_aim_marker: its flight probed against the scene's depth, from
// runtime/hle/vr_slingshot.c): a small ring in a quad layer of its own, facing the left eye, a steady size in view.
// Its image is static: drawn once (vr_marker_fill, from the eye copies' command buffer).
enum { MARKER_PX = 64 };
#define MARKER_ANGLE 0.03f  // (its width / its distance: about 1.7 degrees)
static XrSwapchain marker_swapchain;
static XrSwapchainImageVulkanKHR marker_image;
static VkBuffer marker_buffer;
static VkDeviceMemory marker_buffer_memory;
static bool marker_failed, marker_ready;
void vr_marker_destroy(void) {
    if (vk_device) {
        if (marker_buffer) vkDestroyBuffer(vk_device, marker_buffer, NULL);
        if (marker_buffer_memory) vkFreeMemory(vk_device, marker_buffer_memory, NULL);
    }
    marker_buffer = VK_NULL_HANDLE, marker_buffer_memory = VK_NULL_HANDLE;
    if (marker_swapchain && p_xrDestroySwapchain) p_xrDestroySwapchain(marker_swapchain);
    marker_swapchain = XR_NULL_HANDLE;
    marker_failed = marker_ready = false;
}

void vr_marker_fill(VkCommandBuffer cb) {
    if (marker_ready || marker_failed || !xr_session || swapchain_format == VK_FORMAT_UNDEFINED) return;
    marker_failed = true;  // (until it all succeeds)
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT,
                                .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | MIRROR_USAGE,
                                .format = swapchain_format, .sampleCount = 1, .width = MARKER_PX, .height = MARKER_PX,
                                .faceCount = 1, .arraySize = 1, .mipCount = 1};
    uint32_t n = 1;
    marker_image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    if (!xr_ok(p_xrCreateSwapchain(xr_session, &ci, &marker_swapchain), "xrCreateSwapchain (marker)") ||
        !xr_ok(p_xrEnumerateSwapchainImages(marker_swapchain, 1, &n, (XrSwapchainImageBaseHeader *)&marker_image),
               "xrEnumerateSwapchainImages (marker)"))
        return;
    const VkDeviceSize size = MARKER_PX * MARKER_PX * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk_device, &bi, NULL, &marker_buffer) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_device, marker_buffer, &mr);
    const uint32_t type = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    uint8_t *px;
    if (type == UINT32_MAX || vkAllocateMemory(vk_device, &ai, NULL, &marker_buffer_memory) != VK_SUCCESS ||
        vkBindBufferMemory(vk_device, marker_buffer, marker_buffer_memory, 0) != VK_SUCCESS ||
        vkMapMemory(vk_device, marker_buffer_memory, 0, size, 0, (void **)&px) != VK_SUCCESS)
        return;
    // A white ring with a dark edge either side and a dot in the middle, premultiplied, its edges smoothed.
    const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format == VK_FORMAT_B8G8R8A8_UNORM;
    for (int y = 0; y < MARKER_PX; y++)
        for (int x = 0; x < MARKER_PX; x++) {
            const float dx = (x + 0.5f) / MARKER_PX * 2 - 1, dy = (y + 0.5f) / MARKER_PX * 2 - 1, r = sqrtf(dx * dx + dy * dy);
            const float px1 = 2.0f / MARKER_PX;  // (one pixel, in the -1..1 square)
            #define BAND(lo, hi) fmaxf(0.0f, fminf(1.0f, fminf((r - (lo)) / px1 + 0.5f, ((hi) - r) / px1 + 0.5f)))
            const float white = fmaxf(BAND(0.62f, 0.80f), BAND(-1.0f, 0.13f));
            const float dark = fmaxf(BAND(0.52f, 0.92f), BAND(-1.0f, 0.22f));
            #undef BAND
            const float a = fmaxf(white, dark * 0.65f), c = white;  // (premultiplied: colour <= alpha)
            uint8_t *p = px + (y * MARKER_PX + x) * 4;
            const uint8_t v = (uint8_t)(c * 255), al = (uint8_t)(a * 255);
            p[0] = bgra ? v : v, p[1] = v, p[2] = v, p[3] = al;
        }
    vkUnmapMemory(vk_device, marker_buffer_memory);
    uint32_t index;
    if (!xr_ok(p_xrAcquireSwapchainImage(marker_swapchain, &(XrSwapchainImageAcquireInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}, &index),
               "xrAcquireSwapchainImage (marker)") ||
        !xr_ok(p_xrWaitSwapchainImage(marker_swapchain, &(XrSwapchainImageWaitInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                                                                                      .timeout = XR_INFINITE_DURATION}),
               "xrWaitSwapchainImage (marker)"))
        return;
    barrier_layers(cb, marker_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {MARKER_PX, MARKER_PX, 1}};
    vkCmdCopyBufferToImage(cb, marker_buffer, marker_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier_layers(cb, marker_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 1);
    marker_failed = false;
    marker_ready = true;  // (released at this frame's end: vr_frame_end)
    LOG("[vr] Slingshot marker image ready\n");
}

// The marker's quad this frame: at the landing point, as eye 0 sees it - the game point in eye 0's own frame (the
// rig's view of the world from it, vr_rig_cull_view, in game units) taken to the tracking space from eye 0's pose -
// facing that eye, its size a steady angle. False: nothing to mark.
static bool marker_pose(XrPosef *pose, float *size) {
    extern bool vr_aim_marker(float p[3]);
    extern bool vr_rig_cull_view(float view[12], float tangents[4]);
    extern float vr_rig_units_per_metre(void);
    float P[3], V[12], tan4[4];
    if (!marker_ready || !vr_aim_marker(P) || !vr_rig_cull_view(V, tan4)) return false;
    const float u = vr_rig_units_per_metre();
    if (!(u > 0)) return false;
    float e[3];  // (eye 0's frame: x right, y up, -z ahead; metres)
    for (int r = 0; r < 3; r++) e[r] = (V[r * 4] * P[0] + V[r * 4 + 1] * P[1] + V[r * 4 + 2] * P[2] + V[r * 4 + 3]) / u;
    const float dist = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (!(e[2] < -0.05f) || !isfinite(dist)) return false;  // (behind the eye)
    const XrPosef *eye = &frame.views[0].pose;
    const XrQuaternionf q = eye->orientation;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    const float lp[3] = {eye->position.x + R[0] * e[0] + R[1] * e[1] + R[2] * e[2],
                         eye->position.y + R[3] * e[0] + R[4] * e[1] + R[5] * e[2],
                         eye->position.z + R[6] * e[0] + R[7] * e[1] + R[8] * e[2]};
    *pose = (XrPosef){q, {lp[0], lp[1], lp[2]}};
    *size = dist * MARKER_ANGLE;
    return true;
}

// (device.c, VR present, the menu up) The menu's image (ImGui, top-down, premultiplied; layout GENERAL) into the
// menu panel's swapchain.
bool vr_record_menu_copy(void *command_buffer, void *menu_image, uint32_t w, uint32_t h) {
    if (!vr_stereo_active() || menu_acquired || !command_buffer || !menu_image || !w || !h || !menu_create()) return false;
    VkCommandBuffer cb = command_buffer;
    static int acquire_logged;
    if (!xr_frame_ok(p_xrAcquireSwapchainImage(menu_swapchain, &(XrSwapchainImageAcquireInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO},
                                               &menu_image_index), "xrAcquireSwapchainImage (menu)", &acquire_logged) ||
        menu_image_index >= menu_count)
        return false;
    XrSwapchainImageWaitInfo wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000ll};
    XrResult r = XR_TIMEOUT_EXPIRED;
    for (int tries = 0; r == XR_TIMEOUT_EXPIRED && tries < 20; tries++) r = p_xrWaitSwapchainImage(menu_swapchain, &wait);
    if (r != XR_SUCCESS) return false;
    menu_acquired = true;
    const bool fill_beam = !beam_ready;
    if (fill_beam) beam_fill(cb);
    VkImage dst = menu_images[menu_image_index].image, blit_dst = menu_staging ? menu_staging : dst;
    if (menu_staging) barrier_layers(cb, menu_staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    barrier_layers(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {MENU_W, MENU_H, 1}}};
    vkCmdBlitImage(cb, menu_image, VK_IMAGE_LAYOUT_GENERAL, blit_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    if (menu_staging) {
        barrier_layers(cb, menu_staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 1);
        const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {MENU_W, MENU_H, 1}};
        vkCmdCopyImage(cb, menu_staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    barrier_layers(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, 1);
    menu_copied = true;
    return true;
}

static XrQuaternionf quat_from_columns(const float x[3], const float y[3], const float z[3]) {
    const float r00 = x[0], r01 = y[0], r02 = z[0], r10 = x[1], r11 = y[1], r12 = z[1], r20 = x[2], r21 = y[2], r22 = z[2];
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        const float s4 = sqrtf(tr + 1) * 2;
        return (XrQuaternionf){(r21 - r12) / s4, (r02 - r20) / s4, (r10 - r01) / s4, s4 / 4};
    } else if (r00 > r11 && r00 > r22) {
        const float s4 = sqrtf(1 + r00 - r11 - r22) * 2;
        return (XrQuaternionf){s4 / 4, (r01 + r10) / s4, (r02 + r20) / s4, (r21 - r12) / s4};
    } else if (r11 > r22) {
        const float s4 = sqrtf(1 + r11 - r00 - r22) * 2;
        return (XrQuaternionf){(r01 + r10) / s4, s4 / 4, (r12 + r21) / s4, (r02 - r20) / s4};
    }
    const float s4 = sqrtf(1 + r22 - r00 - r11) * 2;
    return (XrQuaternionf){(r02 + r20) / s4, (r12 + r21) / s4, s4 / 4, (r10 - r01) / s4};
}

// Each frame (vr_frame_end, and before the copy): place the panel when the menu comes up; while it is up, it is what
// the controller's ray aims at.
static float g_hud_world[16];  // (defined with hud_pose below)
static bool g_hud_world_ok;
static bool g_menu_passive;  // the panel shows only the overlays (menu closed): it follows the HUD panel, no pointer
static void menu_panel_update(void) {
    extern bool menu_on_vr_panel(void), menu_vr_passive(void);
    extern uint64_t menu_vr_panel_generation(void);
    static uint64_t generation;
    const uint64_t next_generation = menu_vr_panel_generation();
    if (generation != next_generation) {
        generation = next_generation;
        g_menu_placed = false;
    }
    g_menu_passive = !menu_on_vr_panel() && menu_vr_passive();
    if (g_menu_passive) {  // (where the HUD panel is, as wide; columns made unit)
        if (!g_hud_world_ok) { g_menu_placed = false; return; }
        memcpy(g_menu_world, g_hud_world, sizeof g_menu_world);
        for (int k = 0; k < 3; k++) {
            const float l = sqrtf(g_menu_world[k] * g_menu_world[k] + g_menu_world[4 + k] * g_menu_world[4 + k] +
                                  g_menu_world[8 + k] * g_menu_world[8 + k]);
            if (l > 1e-6f) for (int r = 0; r < 3; r++) g_menu_world[r * 4 + k] /= l;
        }
        g_menu_placed = g_menu_was_passive = true;
        return;
    }
    if (!menu_on_vr_panel()) { g_menu_placed = false; return; }
    if (g_menu_placed && g_menu_was_passive) g_menu_placed = false;  // (the menu opened over the overlays: re-place)
    g_menu_was_passive = false;
    if (!frame.stereo && !g_menu_placed) return;
    if (!g_menu_placed) {  // level, in front of the gaze (yaw only)
        const XrPosef *a = &frame.views[0].pose, *b = &frame.views[1].pose;
        const float x = a->orientation.x, y = a->orientation.y, z = a->orientation.z, w = a->orientation.w;
        float fx = -(2 * (x * z + y * w)), fz = -(1 - 2 * (x * x + y * y));  // (-Z of the head, flattened)
        const float len = sqrtf(fx * fx + fz * fz);
        if (len > 1e-4f) fx /= len, fz /= len; else fx = 0, fz = -1;
        const float d = g_vr_hud_config.menu_distance;
        const float c[3] = {(a->position.x + b->position.x) / 2 + fx * d, (a->position.y + b->position.y) / 2 + g_vr_hud_config.menu_height,
                            (a->position.z + b->position.z) / 2 + fz * d};
        // columns: right = (-fz, 0, fx), up = (0, 1, 0), back (toward the player) = (-fx, 0, -fz)
        const float m[16] = {-fz, 0, -fx, c[0], 0, 1, 0, c[1], fx, 0, -fz, c[2], 0, 0, 0, 1};
        memcpy(g_menu_world, m, sizeof m);
        LOG("[vr] menu panel placed for dialog %llu at (%.2f %.2f %.2f)\n",
            (unsigned long long)generation, c[0], c[1], c[2]);
        g_menu_placed = true;
    }
    const float hw = g_vr_hud_config.menu_width * 0.5f, hh = hw * MENU_H / MENU_W;
    float h[16];
    memcpy(h, g_menu_world, sizeof h);
    for (int r = 0; r < 3; r++) h[r * 4 + 0] *= hw, h[r * 4 + 1] *= hh;
    vr_controls_set_hud_panel(h);
}

void vr_menu_panel_frame(void);
void vr_menu_panel_frame(void) { menu_panel_update(); }

static XrPosef menu_pose(void) {
    const float *m = g_menu_world;
    const float x[3] = {m[0], m[4], m[8]}, y[3] = {m[1], m[5], m[9]}, z[3] = {m[2], m[6], m[10]};
    return (XrPosef){quat_from_columns(x, y, z), {m[3], m[7], m[11]}};
}

// The beam's quad: from the controller along its ray to the hit (or 1.5 m), turned about the ray to face the head.
static bool beam_pose(XrPosef *pose, float *length) {
    extern bool vr_menu_beam(float o[3], float d[3], float *len);
    float o[3], d[3], len;
    if (!beam_ready || !vr_menu_beam(o, d, &len)) return false;
    const XrPosef *a = &frame.views[0].pose, *b = &frame.views[1].pose;
    const float c[3] = {o[0] + d[0] * len / 2, o[1] + d[1] * len / 2, o[2] + d[2] * len / 2};
    const float to_head[3] = {(a->position.x + b->position.x) / 2 - c[0], (a->position.y + b->position.y) / 2 - c[1],
                              (a->position.z + b->position.z) / 2 - c[2]};
    float x[3] = {d[1] * to_head[2] - d[2] * to_head[1], d[2] * to_head[0] - d[0] * to_head[2], d[0] * to_head[1] - d[1] * to_head[0]};
    const float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(xl > 1e-5f)) return false;
    for (int i = 0; i < 3; i++) x[i] /= xl;
    const float z[3] = {x[1] * d[2] - x[2] * d[1], x[2] * d[0] - x[0] * d[2], x[0] * d[1] - x[1] * d[0]};
    *pose = (XrPosef){quat_from_columns(x, d, z), {c[0], c[1], c[2]}};
    *length = len;
    return true;
}

// (the desktop mirror) Where the left eye sees the menu panel: as vr_mirror_hud_mvp.
static float menu_panel_width(void) { return g_menu_passive ? 1.6f * g_vr_hud_config.hud_scale : g_vr_hud_config.menu_width; }
bool vr_mirror_menu_mvp(float out[16]) {
    if (!g_menu_placed) return false;
    const float hw = menu_panel_width() * 0.5f;
    return mirror_quad_mvp(g_menu_world, hw, hw * MENU_H / MENU_W, out);
}

static float g_hud_world[16];  // (hud_pose) the panel last placed: world-from-panel, columns right, up, back (scaled)
static bool g_hud_world_ok;

// (device.c, the desktop mirror) The panel as the left eye sees it this frame: column-major clip-from-panel, the
// panel's (x, y) in -1..1 over its half extents (0.8 x 0.45 m at HUD scale 1). False: no stereo frame or no panel.
bool vr_mirror_hud_mvp(float out[16]) {
    if (!g_hud_world_ok) return false;
    return mirror_quad_mvp(g_hud_world, 0.8f * hud_size_factor(), 0.45f * hud_size_factor(), out);
}

// m: world-from-quad (columns right, up, back, centre); hx, hy: the half extents along its first two columns.
static bool mirror_quad_mvp(const float m[16], float hx, float hy, float out[16]) { return eye_quad_mvp(0, m, hx, hy, out); }
static bool eye_quad_mvp(int eye, const float m[16], float hx, float hy, float out[16]) {
    if (!frame.begun || !frame.stereo) return false;
    const XrPosef *e = &frame.views[eye].pose;
    const XrFovf *f = &frame.views[eye].fov;
    const float x = e->orientation.x, y = e->orientation.y, z = e->orientation.z, w = e->orientation.w;
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    const float p[3] = {e->position.x, e->position.y, e->position.z};
    // panel point (u, v) -> world: c + u * 0.8 * X + v * 0.45 * Y; -> eye: R^T (q - p)
    float cols[3][3];  // eye-space images of the panel's x half extent, y half extent, centre
    const float ax[3] = {m[0] * hx, m[4] * hx, m[8] * hx}, ay[3] = {m[1] * hy, m[5] * hy, m[9] * hy};
    const float c[3] = {m[3] - p[0], m[7] - p[1], m[11] - p[2]};
    for (int r = 0; r < 3; r++) {
        cols[0][r] = R[0 * 3 + r] * ax[0] + R[1 * 3 + r] * ax[1] + R[2 * 3 + r] * ax[2];
        cols[1][r] = R[0 * 3 + r] * ay[0] + R[1 * 3 + r] * ay[1] + R[2 * 3 + r] * ay[2];
        cols[2][r] = R[0 * 3 + r] * c[0] + R[1 * 3 + r] * c[1] + R[2 * 3 + r] * c[2];
    }
    const float l = tanf(f->angleLeft), rt = tanf(f->angleRight), u = tanf(f->angleUp), d = tanf(f->angleDown);
    // eye (ex, ey, ez) -> clip: x = (2 ex + (rt + l) ez) / (rt - l), y (down) = -(2 ey + (u + d) ez) / (u - d), z = 0,
    // w = -ez (in front: ez < 0)
    const float P[4][3] = {{2 / (rt - l), 0, (rt + l) / (rt - l)}, {0, -2 / (u - d), -(u + d) / (u - d)}, {0, 0, 0}, {0, 0, -1}};
    // out (column-major): column k = clip of basis k: (ax, 0), (ay, 0), (0 for z), (centre, 1)
    for (int row = 0; row < 4; row++) {
        out[0 * 4 + row] = P[row][0] * cols[0][0] + P[row][1] * cols[0][1] + P[row][2] * cols[0][2];
        out[1 * 4 + row] = P[row][0] * cols[1][0] + P[row][1] * cols[1][1] + P[row][2] * cols[1][2];
        out[2 * 4 + row] = 0;
        out[3 * 4 + row] = P[row][0] * cols[2][0] + P[row][1] * cols[2][1] + P[row][2] * cols[2][2];
    }
    return true;
}

// The quad layer's pose: vr_hud.h's world-from-HUD matrix from the head (between the eyes), in the local space.
static XrPosef hud_pose(void) {
    const XrPosef *a = &frame.views[0].pose, *b = &frame.views[1].pose;
    const float x = a->orientation.x, y = a->orientation.y, z = a->orientation.z, w = a->orientation.w;
    const float head[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), (a->position.x + b->position.x) / 2,
                            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), (a->position.y + b->position.y) / 2,
                            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), (a->position.z + b->position.z) / 2,
                            0, 0, 0, 1};
    float m[16];
    const float dt = frame.display_period > 0 ? (float)frame.display_period * 1e-9f : 1.0f / 90.0f;
    extern bool vr_menu_pointer_active(void);  // (vr_controls.c: the panel holds still while it is in use)
    extern bool menu_is_open(void), menu_on_vr_panel(void);
    extern bool vr_game_paused_recently(void);  // (vr_controls.c: a closing menu fades out after the pause ends)
    g_vr_hud_hold = vr_menu_pointer_active() || g_cinema || vr_game_paused_recently();  // (a cutscene's screen too)
    ComputeHUDQuadMatrixDelta(m, head, NULL, dt);
    memcpy(g_hud_world, m, sizeof g_hud_world);
    g_hud_world_ok = true;
    {   // (the menu pointer, vr_controls.c) the panel with its half extents as axes
        const float hw = 0.8f * g_vr_hud_config.hud_scale * hud_size_factor(), hh = hw * HUD_H / HUD_W;
        float h[16];
        memcpy(h, m, sizeof h);
        for (int r = 0; r < 3; r++) h[r * 4 + 0] *= hw, h[r * 4 + 1] *= hh;
        if (!menu_on_vr_panel()) vr_controls_set_hud_panel(h);  // (else the menu's panel: menu_panel_update)
        static uint64_t next;  // (diagnostics, with the menu pointer's [vr-pointer] lines) the head and the panel
        const uint64_t now = SDL_GetTicksNS();
        if (now >= next) {
            next = now + 500000000ull;
            LOG("[vr-panel] head (%.2f %.2f %.2f) looking (%.2f %.2f %.2f) | panel centre (%.2f %.2f %.2f) facing (%.2f %.2f %.2f) "
                "| distance %.2f offset %.2f scale %.2f lock %d\n", head[3], head[7], head[11], -head[2], -head[6], -head[10],
                m[3], m[7], m[11], m[2], m[6], m[10], g_vr_hud_config.hud_distance, g_vr_hud_config.hud_vertical_offset,
                g_vr_hud_config.hud_scale, (int)g_vr_hud_config.lock_to_head);
        }
    }
    // rotation (columns right, up, back) -> quaternion
    const float r00 = m[0], r01 = m[1], r02 = m[2], r10 = m[4], r11 = m[5], r12 = m[6], r20 = m[8], r21 = m[9], r22 = m[10];
    XrQuaternionf q;
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        const float s4 = sqrtf(tr + 1) * 2;
        q = (XrQuaternionf){(r21 - r12) / s4, (r02 - r20) / s4, (r10 - r01) / s4, s4 / 4};
    } else if (r00 > r11 && r00 > r22) {
        const float s4 = sqrtf(1 + r00 - r11 - r22) * 2;
        q = (XrQuaternionf){s4 / 4, (r01 + r10) / s4, (r02 + r20) / s4, (r21 - r12) / s4};
    } else if (r11 > r22) {
        const float s4 = sqrtf(1 + r11 - r00 - r22) * 2;
        q = (XrQuaternionf){(r01 + r10) / s4, s4 / 4, (r12 + r21) / s4, (r02 - r20) / s4};
    } else {
        const float s4 = sqrtf(1 + r22 - r00 - r11) * 2;
        q = (XrQuaternionf){(r02 + r20) / s4, (r12 + r21) / s4, s4 / 4, (r10 - r01) / s4};
    }
    return (XrPosef){.orientation = q, .position = {m[3], m[7], m[11]}};
}

static XrVector3f rotate_vector(XrQuaternionf q, XrVector3f v) {
    const XrVector3f u = {q.x, q.y, q.z};
    const float dot = u.x * v.x + u.y * v.y + u.z * v.z;
    const float uu = u.x * u.x + u.y * u.y + u.z * u.z;
    const XrVector3f cross = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return (XrVector3f){2 * dot * u.x + (q.w * q.w - uu) * v.x + 2 * q.w * cross.x,
                        2 * dot * u.y + (q.w * q.w - uu) * v.y + 2 * q.w * cross.y,
                        2 * dot * u.z + (q.w * q.w - uu) * v.z + 2 * q.w * cross.z};
}

static XrQuaternionf basis_quaternion(XrVector3f right, XrVector3f up, XrVector3f back) {
    const float r00 = right.x, r01 = up.x, r02 = back.x;
    const float r10 = right.y, r11 = up.y, r12 = back.y;
    const float r20 = right.z, r21 = up.z, r22 = back.z;
    const float tr = r00 + r11 + r22;
    if (tr > 0) { const float s = sqrtf(tr + 1) * 2; return (XrQuaternionf){(r21-r12)/s,(r02-r20)/s,(r10-r01)/s,s/4}; }
    if (r00 > r11 && r00 > r22) { const float s = sqrtf(1+r00-r11-r22)*2; return (XrQuaternionf){s/4,(r01+r10)/s,(r02+r20)/s,(r21-r12)/s}; }
    if (r11 > r22) { const float s = sqrtf(1+r11-r00-r22)*2; return (XrQuaternionf){(r01+r10)/s,s/4,(r12+r21)/s,(r02-r20)/s}; }
    const float s = sqrtf(1+r22-r00-r11)*2; return (XrQuaternionf){(r02+r20)/s,(r12+r21)/s,s/4,(r10-r01)/s};
}

static XrQuaternionf multiply_quaternion(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                           a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                           a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                           a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}

static XrPosef wrist_hud_pose(int hand) {
    const XrPosef grip = hud_grip_pose[hand];
    XrVector3f local = {(hand ? 1.0f : -1.0f) * g_vr_hud_config.wrist_hud_side,
                        g_vr_hud_config.wrist_hud_up, g_vr_hud_config.wrist_hud_forward};
    XrVector3f offset = rotate_vector(grip.orientation, local);
    XrVector3f position = {grip.position.x + offset.x, grip.position.y + offset.y, grip.position.z + offset.z};
    // The neutral OpenXR grip is a handshake pose: fingers forward and thumb up. A useful wrist display therefore
    // faces along the grip's +Y (thumb-up) axis. Using the anatomical back-of-hand normal (+/-X) would leave the
    // display facing sideways until the player unnaturally rolled the forearm palm-down.
    XrVector3f back = rotate_vector(grip.orientation, (XrVector3f){0, 1.0f, 0});
    XrVector3f along_arm = rotate_vector(grip.orientation, (XrVector3f){0, 0, -1});
    XrVector3f right = {along_arm.y*back.z-along_arm.z*back.y,
                        along_arm.z*back.x-along_arm.x*back.z,
                        along_arm.x*back.y-along_arm.y*back.x};
    float length = sqrtf(right.x*right.x + right.y*right.y + right.z*right.z);
    if (length < 1e-4f) right = (XrVector3f){1,0,0}; else right.x/=length, right.y/=length, right.z/=length;
    XrVector3f up = {back.y*right.z-back.z*right.y, back.z*right.x-back.x*right.z,
                     back.x*right.y-back.y*right.x};
    XrQuaternionf orientation = basis_quaternion(right, up, back);
    const float half_tilt = g_vr_hud_config.wrist_hud_tilt * (float)M_PI / 360.0f;
    orientation = multiply_quaternion(orientation, (XrQuaternionf){sinf(half_tilt), 0, 0, cosf(half_tilt)});
    return (XrPosef){orientation, position};
}

static bool wrist_hud_glanced(int hand, XrPosef panel) {
    const XrPosef *a = &frame.views[0].pose, *b = &frame.views[1].pose;
    XrVector3f to_head = {(a->position.x+b->position.x)*.5f-panel.position.x,
                          (a->position.y+b->position.y)*.5f-panel.position.y,
                          (a->position.z+b->position.z)*.5f-panel.position.z};
    float length = sqrtf(to_head.x*to_head.x + to_head.y*to_head.y + to_head.z*to_head.z);
    if (length < 1e-4f) return true;
    to_head.x/=length; to_head.y/=length; to_head.z/=length;
    XrVector3f hand_back = rotate_vector(hud_grip_pose[hand].orientation, (XrVector3f){0, 1.0f, 0});
    XrVector3f head_forward = rotate_vector(a->orientation, (XrVector3f){0, 0, -1});
    const float hand_facing = hand_back.x*to_head.x + hand_back.y*to_head.y + hand_back.z*to_head.z;
    const float head_gaze = -(head_forward.x*to_head.x + head_forward.y*to_head.y + head_forward.z*to_head.z);
    return hand_facing > .25f && head_gaze > .87f;
}

// ---- this frame's composition layers, built once: xrEndFrame submits them, and the desktop mirror draws the very
// same list (vr_mirror_layers) - so whatever the headset shows, the mirror shows, without per-layer mirror code.
// Built from what was acquired and copied this frame; vr_frame_end then drops any whose release failed.
static struct {
    bool built, proj, hud, menu, beam, use_wrist, panel_in_eyes, marker;
    uint32_t wrist_count;
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer;
    XrCompositionLayerQuad quad, quad_full, menu_quad, beam_quad, wrist[2], fade_layer, marker_quad;
} fl;
// The wrist panels' element groups in the 1920x1080 2D layout (exec.c routes their draws to the wrist texture;
// measured in Link's room): the status group - the hearts (two rows when he has many) - and the item group - the
// rupee counter and Fi's prompt with the D-pad.
#define WRIST_STATUS_RECT 20, 20, 640, 150
#define WRIST_ITEM_RECT 30, 740, 240, 320
// The card: the status group on top, the item group below it (left-aligned), a margin around (layout pixels)
#define WRIST_CARD_MARGIN 16
#define WRIST_CARD_W (640 + 2 * WRIST_CARD_MARGIN)
#define WRIST_CARD_H (150 + 320 + 2 * WRIST_CARD_MARGIN)
static _Atomic bool g_wrist_elements;
bool vr_wrist_elements(void) { return atomic_load(&g_wrist_elements); }  // (exec.c: route them to the wrist texture)

static void build_layers(void) {
    if (fl.built) return;
    fl.built = true;
    fl.panel_in_eyes = false;
    for (uint32_t eye = 0; eye < 2; eye++)
        fl.pv[eye] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = frame.frozen_used ? frozen_views[eye].pose : frame.views[eye].pose,
            .fov = frame.frozen_used ? frozen_views[eye].fov : frame.views[eye].fov,
            .subImage = {.swapchain = swapchain, .imageRect = {{0, 0}, {(int32_t)eye_width, (int32_t)eye_height}},
                         .imageArrayIndex = eye}};
    fl.layer = (XrCompositionLayerProjection){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = local_space,
                                              .viewCount = 2, .views = fl.pv};
    fl.proj = frame.acquired && frame.copied;
    // The HUD panel (vr_record_hud_copy): over the eyes, blended by its (premultiplied) alpha; opaque for a
    // full-screen 2D frame (the map), in front of the frozen world.
    fl.hud = frame.hud_acquired && frame.hud_copied;
    const float hud_w = 1.6f * g_vr_hud_config.hud_scale * hud_size_factor();
    fl.quad = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
                                       .layerFlags = frame.hud_opaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                       .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                       .subImage = {.swapchain = hud_swapchain, .imageRect = {{0, 0}, {HUD_W, HUD_H}}, .imageArrayIndex = 0},
                                       .pose = fl.hud ? hud_pose() : (XrPosef){{0, 0, 0, 1}, {0, 0, 0}},
                                       .size = {hud_w, hud_w * HUD_H / HUD_W}};
    // The settings menu's panel and the beam (menu_panel_update, vr_record_menu_copy), in front of everything.
    fl.menu = menu_acquired && menu_copied && g_menu_placed;
    const float menu_w = menu_panel_width();
    fl.menu_quad = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {.swapchain = menu_swapchain, .imageRect = {{0, 0}, {MENU_W, MENU_H}}, .imageArrayIndex = 0},
        .pose = fl.menu ? menu_pose() : (XrPosef){{0, 0, 0, 1}, {0, 0, 0}}, .size = {menu_w, menu_w * MENU_H / MENU_W}};
    XrPosef bp;
    float beam_len = 0;
    fl.beam = fl.menu && !g_menu_passive && beam_pose(&bp, &beam_len);
    fl.beam_quad = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {.swapchain = beam_swapchain, .imageRect = {{0, 0}, {BEAM_W, BEAM_H}}, .imageArrayIndex = 0},
        .pose = fl.beam ? bp : (XrPosef){{0, 0, 0, 1}, {0, 0, 0}}, .size = {0.006f, beam_len}};
    XrPosef mp;  // (the Slingshot's marker, where its seed will land)
    float marker_size = 0;
    fl.marker = !fl.menu && marker_pose(&mp, &marker_size);
    fl.marker_quad = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {.swapchain = marker_swapchain, .imageRect = {{0, 0}, {MARKER_PX, MARKER_PX}}, .imageArrayIndex = 0},
        .pose = fl.marker ? mp : (XrPosef){{0, 0, 0, 1}, {0, 0, 0}}, .size = {marker_size, marker_size}};
    fl.wrist_count = 0;
    extern bool player_eye(float eye[3]);
    const bool gameplay = player_eye(NULL);  // false on boot, title, loading, and file-select screens
    extern bool vr_game_paused(void);  // (vr_controls.c: a menu pauses the game - its screens stay whole)
    fl.use_wrist = fl.hud && !frame.hud_opaque && g_vr_hud_config.wrist_hud && gameplay && !vr_game_paused() &&
                   (hud_grip_tracked[0] || hud_grip_tracked[1]);
    fl.quad_full = fl.quad;  // (the whole HUD image: the eyes' panel, its wrist corners cut out - vr_eye_panel)
    if (fl.use_wrist) {
        const int status_hand = g_vr_hud_config.wrist_hud_swap_hands ? 1 : 0;
        const int item_hand = 1 - status_hand;
        const XrPosef panels[2] = {hud_grip_tracked[0] ? wrist_hud_pose(0) : (XrPosef){{0,0,0,1},{0,0,0}},
                                   hud_grip_tracked[1] ? wrist_hud_pose(1) : (XrPosef){{0,0,0,1},{0,0,0}}};
        const bool status_visible = hud_grip_tracked[status_hand] &&
                                    (!g_vr_hud_config.wrist_hud_glance ||
                                     wrist_hud_glanced(status_hand, panels[status_hand]));
        // Drawn into the eyes (vr_eye_wrist): one card on the status hand, from the wrist texture (exec.c draws the
        // hearts, the rupees and Fi's prompt there, whole): the hearts' row on top, the rupees and the prompt below.
        // As quad layers (SKYRECOMP_VR_PANEL_QUAD): the HUD image's top corners on both hands, as before.
        const bool eyes = !getenv("SKYRECOMP_VR_PANEL_QUAD");
        if (eyes) {
            if (status_visible) {
                const float s = g_vr_hud_config.wrist_hud_size;
                fl.wrist[fl.wrist_count++] = (XrCompositionLayerQuad){
                    .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
                    .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                    .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                    .subImage = {.swapchain = hud_swapchain, .imageRect = {{0, 0}, {480, 480}}, .imageArrayIndex = 0},
                    .pose = panels[status_hand], .size = {s, s * WRIST_CARD_H / WRIST_CARD_W}};
            }
        } else {
            const XrPosef pose[2] = {panels[status_hand], panels[item_hand]};
            const bool shown[2] = {status_visible, hud_grip_tracked[item_hand]};
            for (int i = 0; i < 2; i++)
                if (shown[i])
                    fl.wrist[fl.wrist_count++] = (XrCompositionLayerQuad){
                        .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = local_space,
                        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                        .subImage = {.swapchain = hud_swapchain,
                                     .imageRect = i ? (XrRect2Di){{1440, 0}, {480, 480}} : (XrRect2Di){{0, 0}, {480, 480}},
                                     .imageArrayIndex = 0},
                        .pose = pose[i], .size = {g_vr_hud_config.wrist_hud_size, g_vr_hud_config.wrist_hud_size}};
        }
        if (!eyes) {
            fl.quad.subImage.imageRect = (XrRect2Di){{480, 0}, {960, HUD_H}};
            fl.quad.size.width = hud_w * 960.0f / HUD_W;
        }
    }
    atomic_store(&g_wrist_elements, fl.use_wrist && !getenv("SKYRECOMP_VR_PANEL_QUAD"));
    // Use the overlay path for the final veil, just like the HUD/keyboard it must cover. A second scene
    // projection layer is not sufficient to keep the veil in front of SteamVR's separately composed overlays.
    // Head-locked and close to the eyes, with far more angular coverage than the headset FOV: no visible edges.
    fl.fade_layer = (XrCompositionLayerQuad){.type=XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, .space=view_space,
        .eyeVisibility=XR_EYE_VISIBILITY_BOTH,
        .subImage={.swapchain=view_fade_swapchain, .imageRect={{0,0},{8,8}}, .imageArrayIndex=0},
        .pose={{0,0,0,1},{0,0,-VR_FADE_OVERLAY_DISTANCE}}, .size={VR_FADE_OVERLAY_SIZE,VR_FADE_OVERLAY_SIZE}};
}

// (device.c, the desktop mirror) The left eye's view of a quad layer: column-major clip-from-quad over its (x, y) in
// -1..1. view: the pose is in view space (head-locked: the head between the eyes, facing as eye 0).
static bool layer_quad_mvp_eye(int eye, const XrCompositionLayerQuad *q, bool view, float out[16]);
static bool layer_quad_mvp(const XrCompositionLayerQuad *q, bool view, float out[16]) { return layer_quad_mvp_eye(0, q, view, out); }
static bool layer_quad_mvp_eye(int eye, const XrCompositionLayerQuad *q, bool view, float out[16]) {
    XrPosef p = q->pose;
    if (view) {
        const XrPosef *a = &frame.views[0].pose, *b = &frame.views[1].pose;
        const XrVector3f head = {(a->position.x + b->position.x) / 2, (a->position.y + b->position.y) / 2,
                                 (a->position.z + b->position.z) / 2};
        const XrVector3f o = rotate_vector(a->orientation, p.position);
        p.position = (XrVector3f){head.x + o.x, head.y + o.y, head.z + o.z};
        p.orientation = multiply_quaternion(a->orientation, p.orientation);
    }
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    // world-from-quad as mirror_quad_mvp reads it: rows of a 3x4 (columns right, up, back, centre)
    const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), p.position.x,
                         2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), p.position.y,
                         2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), p.position.z,
                         0, 0, 0, 1};
    return eye_quad_mvp(eye, m, q->size.width / 2, q->size.height / 2, out);
}

// The HUD panel drawn into the eye images themselves (device.c, vk_vr_panel_into_eye) instead of submitted as a quad
// layer: SteamVR shows quad layers through its overlay path, which takes their contents on its own schedule - the
// game's cursor on the panel juddered in the headset while the eyes (and the desktop mirror) were smooth. In the eye
// images the panel is part of the frame it was drawn for. SKYRECOMP_VR_PANEL_QUAD=1: the quad layer (as before).
// eye: 0 / 1; rect: the part of the HUD image the panel shows (uv origin, size); opaque: a full-screen 2D frame.
// cut: with the wrist panels on, the HUD image's top corners (WRIST_CUT_W x WRIST_CUT_H, uv) are on the wrists: the
// panel shows everything else (device.c draws it as three pieces) - not just its middle band, which lost what sat
// below the corners (the button prompts at the lower left) and split anything across the band's edges.
bool vr_eye_panel(int eye, void **image, uint32_t *width, uint32_t *height, uint32_t *format, float mvp[16],
                  float rect[4], bool *opaque, float cut[2]) {
    static int quad = -1;
    if (quad < 0) quad = getenv("SKYRECOMP_VR_PANEL_QUAD") != NULL;
    if (quad || eye < 0 || eye > 1 || !frame.begun || !frame.stereo || !frame.acquired || !frame.copied) return false;
    build_layers();
    if (!fl.hud || !layer_quad_mvp_eye(eye, &fl.quad_full, false, mvp)) return false;
    fl.panel_in_eyes = true;
    *image = (void *)images[frame.image].image;
    *width = eye_width, *height = eye_height, *format = (uint32_t)swapchain_format;
    rect[0] = 0, rect[1] = 0, rect[2] = 1, rect[3] = 1;
    cut[0] = cut[1] = 0;  // (the wrist elements are drawn into their own texture: the HUD image is the rest, whole)
    *opaque = frame.hud_opaque;
    return true;
}

// (device.c) The wrist card's piece `piece` as eye `eye` sees it: 0 its backing (the whole card; *backing: the Wrist
// Background opacity), 1 the status group, 2 the item group - clip-from-quad and the uv rect in the wrist texture.
bool vr_eye_wrist(int eye, int piece, float mvp[16], float rect[4], float *backing) {
    float card[16];
    if (eye < 0 || eye > 1 || piece < 0 || piece > 2 || !fl.built || !fl.panel_in_eyes || !fl.use_wrist ||
        !fl.wrist_count || getenv("SKYRECOMP_VR_PANEL_QUAD") || !layer_quad_mvp_eye(eye, &fl.wrist[0], false, card))
        return false;
    *backing = g_vr_hud_config.wrist_hud_background;
    static const int groups[2][4] = {{WRIST_STATUS_RECT}, {WRIST_ITEM_RECT}};
    // the piece in the card (layout pixels: x, y, w, h from its top left)
    float px = 0, py = 0, pw = WRIST_CARD_W, ph = WRIST_CARD_H;
    if (piece) {
        const int *g = groups[piece - 1];
        px = WRIST_CARD_MARGIN, py = piece == 1 ? WRIST_CARD_MARGIN : WRIST_CARD_MARGIN + 150, pw = (float)g[2], ph = (float)g[3];
        for (int k = 0; k < 4; k++) rect[k] = (float)g[k] / (k & 1 ? HUD_H : HUD_W);
    } else rect[0] = rect[1] = 0, rect[2] = rect[3] = 1;
    // its quad in the card's -1..1 (+y up): card * S
    const float sx = pw / WRIST_CARD_W, sy = ph / WRIST_CARD_H;
    const float cx = 2 * px / WRIST_CARD_W - 1 + sx, cy = 1 - 2 * py / WRIST_CARD_H - sy;
    for (int r = 0; r < 4; r++) {
        mvp[0 * 4 + r] = card[0 * 4 + r] * sx;
        mvp[1 * 4 + r] = card[1 * 4 + r] * sy;
        mvp[2 * 4 + r] = card[2 * 4 + r];
        mvp[3 * 4 + r] = card[0 * 4 + r] * cx + card[1 * 4 + r] * cy + card[3 * 4 + r];
    }
    return true;
}

static bool mirror_quad(VrMirrorLayer *l, const XrCompositionLayerQuad *q, VkImage image, uint32_t w, uint32_t h, bool view) {
    if (!image || !layer_quad_mvp(q, view, l->mvp)) return false;
    l->image = (void *)image;
    l->format = (uint32_t)swapchain_format;
    l->width = w, l->height = h, l->layers = 1;
    l->rect[0] = (float)q->subImage.imageRect.offset.x / w, l->rect[1] = (float)q->subImage.imageRect.offset.y / h;
    l->rect[2] = (float)q->subImage.imageRect.extent.width / w, l->rect[3] = (float)q->subImage.imageRect.extent.height / h;
    l->blend = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
    l->full = false;
    return true;
}

// The layers this frame submits, in order, as the left eye sees them (the eye image fills the window).
int vr_mirror_layers(VrMirrorLayer *out, int max) {
    if (!frame.begun || !frame.stereo || max < 9) return 0;
    build_layers();
    if (!fl.proj) return 0;
    int n = 0;
    out[n++] = (VrMirrorLayer){.image = (void *)images[frame.image].image, .format = (uint32_t)swapchain_format,
                               .width = eye_width, .height = eye_height, .layers = 2, .rect = {0, 0, 1, 1}, .full = true};
    if (fl.hud && !fl.panel_in_eyes && mirror_quad(&out[n], &fl.quad, hud_images[frame.hud_image].image, HUD_W, HUD_H, false)) n++;
    for (uint32_t i = 0; fl.use_wrist && !fl.panel_in_eyes && i < fl.wrist_count; i++)
        if (mirror_quad(&out[n], &fl.wrist[i], hud_images[frame.hud_image].image, HUD_W, HUD_H, false)) n++;
    if (fl.menu && mirror_quad(&out[n], &fl.menu_quad, menu_images[menu_image_index].image, MENU_W, MENU_H, false)) n++;
    if (fl.beam && mirror_quad(&out[n], &fl.beam_quad, beam_image.image, BEAM_W, BEAM_H, false)) n++;
    if (view_fade_acquired && view_fade_ready && mirror_quad(&out[n], &fl.fade_layer, view_fade_images[view_fade_index].image, 8, 8, true)) n++;
    return n;
}

// (device.c) The mirror's layer images between sampled (GENERAL) and the layout the runtime takes them back in.
void vr_mirror_barriers(void *command_buffer, const VrMirrorLayer *l, int n, bool to_read) {
    VkCommandBuffer cb = (VkCommandBuffer)command_buffer;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++) seen |= l[j].image == l[i].image;
        if (seen) continue;
        eye_barrier_layers(cb, (VkImage)l[i].image,
                           to_read ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                           to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           to_read ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           to_read ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           l[i].layers);
    }
}

void vr_frame_end(void) {
    if (!frame.begun) return;
    static int release_logged, end_logged;
    static uint64_t frames;
    set_stage(STAGE_END_FRAME);
    build_layers();
    // Release only after the blits were queued (OpenXR orders the compositor's reads after prior submissions on the
    // session's queue; no CPU wait for the GPU is needed).
    bool release_ok = frame.acquired &&
        xr_frame_ok(p_xrReleaseSwapchainImage(swapchain, &(XrSwapchainImageReleaseInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}),
                    "xrReleaseSwapchainImage", &release_logged);
    const bool submit = release_ok && fl.proj;
    static int hud_release_logged;
    const bool hud_ok = frame.hud_acquired &&
        xr_frame_ok(p_xrReleaseSwapchainImage(hud_swapchain, &(XrSwapchainImageReleaseInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}),
                    "xrReleaseSwapchainImage (HUD)", &hud_release_logged) && fl.hud;
    static int menu_release_logged;
    const bool menu_ok = menu_acquired &&
        xr_frame_ok(p_xrReleaseSwapchainImage(menu_swapchain, &(XrSwapchainImageReleaseInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}),
                    "xrReleaseSwapchainImage (menu)", &menu_release_logged) && fl.menu;
    menu_acquired = menu_copied = false;
    {   static bool beam_released;  // (the static image: released once, after its fill was recorded)
        if (beam_ready && !beam_released)
            beam_released = XR_SUCCEEDED(p_xrReleaseSwapchainImage(beam_swapchain, &(XrSwapchainImageReleaseInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}));
        if (!beam_ready) beam_released = false;
    }
    {   static bool marker_released;  // (the static image: released once, after its fill was recorded)
        if (marker_ready && !marker_released)
            marker_released = XR_SUCCEEDED(p_xrReleaseSwapchainImage(marker_swapchain, &(XrSwapchainImageReleaseInfo){.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}));
        if (!marker_ready) marker_released = false;
    }
    const bool beam_ok = menu_ok && fl.beam;
    const bool use_wrist = hud_ok && fl.use_wrist && !fl.panel_in_eyes;
    const XrCompositionLayerBaseHeader *layers[10];
    uint32_t nlayers = 0;
    if (submit) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.layer;
    if (submit && hud_ok && !fl.panel_in_eyes) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.quad;
    if (submit && use_wrist)
        for (uint32_t i = 0; i < fl.wrist_count; i++) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.wrist[i];
    if (submit && menu_ok) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.menu_quad;
    if (submit && beam_ok) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.beam_quad;
    if (submit && fl.marker) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.marker_quad;
    const bool has_view_fade = submit && view_fade_acquired && view_fade_ready;
    static bool had_view_fade;
    if (had_view_fade && !has_view_fade && getenv("SKYRECOMP_VR_FADE_TRACE")) LOG("[vr-fade] compositor fade cleared\n");
    had_view_fade = has_view_fade;
    if (view_fade_acquired) {
        const bool released = xr_ok(p_xrReleaseSwapchainImage(view_fade_swapchain,
            &(XrSwapchainImageReleaseInfo){.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}), "xrReleaseSwapchainImage (fade)");
        if (submit && released && view_fade_ready) layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&fl.fade_layer;
        if (getenv("SKYRECOMP_VR_FADE_TRACE") && view_fade_ready) {
            static unsigned traced;
            if (traced++ % 30 == 0) LOG("[vr-fade] compositor FINAL head-locked QUAD layer %u, both eyes, release %d, submitted %d\n", nlayers, released, submit);
        }
        view_fade_acquired = view_fade_ready = false;
    }
    fl.built = false;
    if (g_vr_config.dynamic_resolution && frame.work_start_ns && frame.display_period > 0) {
        adaptive_work_sum += (double)(SDL_GetTicksNS() - frame.work_start_ns);
        adaptive_budget_sum += (double)frame.display_period;
        if (++adaptive_samples >= 30) {
            const double cpu_load = adaptive_work_sum / adaptive_budget_sum;
            const float gpu_ms = g_vr_gpu_ms;
            const double gpu_load = gpu_ms > 0 ? (double)gpu_ms * 1.0e6 / (double)frame.display_period : 0.0;
            // CPU submission time does not include asynchronous GPU execution. Use whichever side is closer to
            // missing the headset deadline so dynamic resolution actually reacts to fill-rate-bound VR scenes.
            const double load = fmax(cpu_load, gpu_load);
            const float ceiling = configured_resolution_ceiling();
            const float floor = SDL_clamp(g_vr_config.dynamic_resolution_min, 0.5f, ceiling);
            float before = vr_resolution_factor();
            if (load > 0.88) {
                adaptive_resolution = fmaxf(floor, before - (load > 1.05 ? 0.10f : 0.05f));
                adaptive_headroom = 0;
            } else if (load < 0.68 && before < ceiling) {
                if (++adaptive_headroom >= 6) {
                    adaptive_resolution = fminf(ceiling, before + 0.05f);
                    adaptive_headroom = 0;
                }
            } else {
                adaptive_headroom = 0;
            }
            adaptive_resolution = roundf(adaptive_resolution * 20.0f) / 20.0f;
            if (adaptive_resolution != before)
                LOG("[vr] adaptive resolution %.2fx -> %.2fx (load %.0f%%: CPU %.0f%%, GPU %.0f%%)\n", before,
                    adaptive_resolution, load * 100.0, cpu_load * 100.0, gpu_load * 100.0);
            adaptive_work_sum = adaptive_budget_sum = 0;
            adaptive_samples = 0;
        }
    } else if (!g_vr_config.dynamic_resolution) {
        adaptive_resolution = configured_resolution_ceiling();
        adaptive_work_sum = adaptive_budget_sum = 0;
        adaptive_samples = adaptive_headroom = 0;
    }
    XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame.display_time,
                         .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = nlayers,
                         .layers = nlayers ? layers : NULL};
    xr_frame_ok(p_xrEndFrame(xr_session, &ei), "xrEndFrame", &end_logged);
    if (trace_enabled() && !(frames++ % 300)) {
        LOG("[vr] frame %llu: stereo %d, layer %d, camera draws %u/%u (reference %.3f %.3f %.3f %.3f)\n",
            (unsigned long long)frames, frame.stereo, submit, frame.patched[0], frame.patched[1], reference_pose[0],
            reference_pose[1], reference_pose[2], reference_pose[3]);
        for (int e = 0; e < 2; e++) {
            const XrView *v = &frame.views[e];
            LOG("[vr]   eye %d: position %.3f %.3f %.3f, orientation %.3f %.3f %.3f %.3f, fov L%.3f R%.3f U%.3f D%.3f\n", e,
                v->pose.position.x, v->pose.position.y, v->pose.position.z, v->pose.orientation.x, v->pose.orientation.y,
                v->pose.orientation.z, v->pose.orientation.w, v->fov.angleLeft, v->fov.angleRight, v->fov.angleUp,
                v->fov.angleDown);
        }
    }
    memset(&frame, 0, sizeof frame);
    set_stage(STAGE_IDLE);
}

#else  // ================================================================== builds without the OpenXR SDK

bool vr_render_scale(float *sx, float *sy) { (void)sx; (void)sy; return false; }
bool vr_eye_extent(uint32_t *w, uint32_t *h) { (void)w; (void)h; return false; }
bool vr_eye_fov(int eye, float fov[4]) { (void)eye; (void)fov; return false; }
uint32_t vr_hidden_mesh(int eye, const float **xy) { (void)eye; (void)xy; return 0; }
float vr_resolution_factor(void) { return 1.0f; }

bool vr_vulkan_init(void *instance, void *physical_device, void *device, uint32_t queue_family, uint32_t queue_index) {
    (void)instance; (void)physical_device; (void)device; (void)queue_family; (void)queue_index;
    if (vr_requested()) set_status("This build has no OpenXR support; continuing in desktop mode");
    return false;
}
void vr_shutdown(void) {}
void vr_poll(void) {}
bool vr_frame_begin(void) { return false; }
bool vr_stereo_active(void) { return false; }
void vr_recenter(void) {}
void vr_freeze_world(bool freeze, bool frame_is_3d) { (void)freeze; (void)frame_is_3d; }
bool vr_view_fade_available(void) { return false; }
bool vr_record_view_fade(void *command_buffer, const float rgba[4]) {
    (void)command_buffer; (void)rgba; return false;
}
bool vr_record_hud_copy(void *command_buffer, void *hud_image, uint32_t w, uint32_t h, bool flip_y, bool opaque) {
    (void)command_buffer; (void)hud_image; (void)w; (void)h; (void)flip_y; (void)opaque;
    return false;
}
void vr_set_grip_poses(bool left_tracked, const float left[7], bool right_tracked, const float right[7]) {
    (void)left_tracked; (void)left; (void)right_tracked; (void)right;
}
float vr_eye_aspect(void) { return 0.0f; }
bool vr_patch_scene_ubo(uint32_t eye, float out[28], const float *in, uint64_t size) {
    (void)eye; (void)out; (void)in; (void)size;
    return false;
}
bool vr_patch_scene_fragment_ubo(uint32_t eye, float out[28], const float *in, uint64_t size) {
    (void)eye; (void)out; (void)in; (void)size;
    return false;
}
bool vr_record_eye_copies(void *command_buffer, void *left_image, void *right_image, uint32_t sw, uint32_t sh, bool flip_y,
                          uint32_t source_format, uint32_t left_layer, uint32_t right_layer) {
    (void)left_layer; (void)right_layer;
    (void)command_buffer; (void)left_image; (void)right_image; (void)sw; (void)sh; (void)flip_y;
    (void)source_format;
    return false;
}
void vr_frame_end(void) {}
int64_t vr_display_time_now(void) { return 0; }
int vr_mirror_layers(VrMirrorLayer *out, int max) { (void)out; (void)max; return 0; }
bool vr_eye_wrist(int eye, int piece, float mvp[16], float rect[4], float *backing) {
    (void)eye; (void)piece; (void)mvp; (void)rect; (void)backing;
    return false;
}
bool vr_wrist_elements(void) { return false; }
bool vr_eye_panel(int eye, void **image, uint32_t *width, uint32_t *height, uint32_t *format, float mvp[16],
                  float rect[4], bool *opaque, float cut[2]) {
    (void)eye; (void)image; (void)width; (void)height; (void)format; (void)mvp; (void)rect; (void)opaque; (void)cut;
    return false;
}
void vr_mirror_barriers(void *command_buffer, const VrMirrorLayer *layers, int n, bool to_read) {
    (void)command_buffer; (void)layers; (void)n; (void)to_read;
}

#endif
