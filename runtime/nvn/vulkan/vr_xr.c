// VR: the headset through OpenXR (Quest 3 over Link, or any PC OpenXR runtime). See vr.h and docs/vr-report.md.
//
// The OpenXR loader is loaded at run time (libopenxr_loader.dll / openxr_loader.dll next to the game or on PATH): a
// game without VR never needs it. The instance is made before Vulkan's, since the runtime names the instance and
// device extensions it needs and the GPU the headset is on (XR_KHR_vulkan_enable).
//
// A frame (T = the render thread, R = the recorder thread, which owns the queue; record.c):
//   T  vr_xr_frame_next   after a present: events, xrWaitFrame (the headset's pacing), xrLocateViews at the frame's
//                         predicted display time -> the eyes' poses and fields of view for the next frame's translation
//   T  (translation)      the scene camera's block gets each eye's view and projection (vr.c)
//   T  vr_xr_present      both eyes (and the HUD / menu panel) drawn into our own images, submitted; then a packet:
//   R  end_run            xrBeginFrame, per swapchain acquire / wait / copy (a prerecorded command buffer) / release,
//                         xrEndFrame with the poses the frame was rendered with (the runtime reprojects from them)
#include "vr.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../../rt.h"
#include "record.h"
#include "vk.h"

void renderer_submit(void);  // (exec.c)

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ------------------------------------------------------------------ functions
static PFN_xrGetInstanceProcAddr xr_gipa;
#define XR_FUNCS(X) \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrGetInstanceProperties) X(xrResultToString) X(xrStringToPath) \
    X(xrGetVulkanInstanceExtensionsKHR) X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) \
    X(xrGetVulkanGraphicsRequirementsKHR) X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) X(xrDestroyInstance) \
    X(xrCreateReferenceSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrEnumerateSwapchainImages) \
    X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) \
    X(xrEndFrame) X(xrLocateViews) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrLocateSpace) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrCreateActionSpace) X(xrApplyHapticFeedback) \
    X(xrGetCurrentInteractionProfile) X(xrPathToString)
#define XR_DECL(f) static PFN_##f p##f;
XR_FUNCS(XR_DECL)
static PFN_xrEnumerateDisplayRefreshRatesFB pxrEnumerateDisplayRefreshRatesFB;
static PFN_xrRequestDisplayRefreshRateFB pxrRequestDisplayRefreshRateFB;
static PFN_xrEnumerateInstanceExtensionProperties pxrEnumerateInstanceExtensionProperties;
static PFN_xrCreateInstance pxrCreateInstance;

static XrInstance xi;
static XrSystemId xsys;
static XrSession xs;
static XrSpace space_local, space_view;
static XrSessionState xstate = XR_SESSION_STATE_UNKNOWN;
static bool running;      // between xrBeginSession and xrEndSession
static bool have_refresh; // XR_FB_display_refresh_rate
static char inst_ext_buf[2048], dev_ext_buf[2048];
static char sys_name[XR_MAX_SYSTEM_NAME_SIZE];
static XrViewConfigurationView vcv[2];

static const char *xr_str(XrResult r) {
    static char s[XR_MAX_RESULT_STRING_SIZE];
    if (xi && pxrResultToString && pxrResultToString(xi, r, s) == XR_SUCCESS) return s;
    snprintf(s, sizeof s, "%d", r);
    return s;
}
#define XR_OK(call) xr_ok_((call), #call)
static bool xr_ok_(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    static int n;
    if (n++ < 40) LOG("[vr] %s: %s\n", what, xr_str(r));
    return false;
}

static bool load_loader(void) {
#ifdef _WIN32
    HMODULE m = LoadLibraryA("libopenxr_loader.dll");
    if (!m) m = LoadLibraryA("openxr_loader.dll");
    if (!m) return false;
    xr_gipa = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(m, "xrGetInstanceProcAddr");
#else
    void *m = dlopen("libopenxr_loader.so.1", RTLD_NOW);
    if (!m) m = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!m) return false;
    xr_gipa = (PFN_xrGetInstanceProcAddr)dlsym(m, "xrGetInstanceProcAddr");
#endif
    return xr_gipa != NULL;
}

// ------------------------------------------------------------------ instance (before Vulkan's)
static int xr_state = -1;  // -1 not tried, 0 no headset (stereo on the desktop only), 1 instance + system
bool vr_xr_on(void) { return xr_state > 0; }
bool vr_xr_running(void) { return running; }

static bool has_xr_ext(const XrExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// The instance and the headset's system (wait_s: seconds to wait for the headset; 0: one try, quietly). Also after a
// lost session (Link dropped): the Vulkan instance and device made for the first one serve again (same GPU).
static const char *inst_want[8];
static uint32_t inst_nwant;
static bool instance_make(int wait_s) {
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "ARMS Rearmed VR");
    strcpy(ci.applicationInfo.engineName, "LARecomp");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = inst_nwant;
    ci.enabledExtensionNames = inst_want;
    if (pxrCreateInstance(&ci, &xi) != XR_SUCCESS) { xi = XR_NULL_HANDLE; return false; }
#define XR_GET(f) xr_gipa(xi, #f, (PFN_xrVoidFunction *)&p##f);
    XR_FUNCS(XR_GET)
    if (have_refresh) {
        xr_gipa(xi, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&pxrEnumerateDisplayRefreshRatesFB);
        xr_gipa(xi, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&pxrRequestDisplayRefreshRateFB);
    }
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    pxrGetInstanceProperties(xi, &ip);
    if (wait_s) LOG("[vr] OpenXR runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                    XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrResult r = XR_SUCCESS;
    for (int i = 0; i <= wait_s * 2; i++) {
        r = pxrGetSystem(xi, &gi, &xsys);
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE || !wait_s) break;
        if (i == 0) LOG("[vr] waiting for the headset (put it on / connect Link)...\n");
#ifdef _WIN32
        Sleep(500);
#endif
    }
    if (r != XR_SUCCESS) {
        if (wait_s) XR_OK(r), LOG("[vr] no headset: stereo on the desktop only\n");
        pxrDestroyInstance(xi);
        xi = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    pxrGetSystemProperties(xi, xsys, &sp);
    snprintf(sys_name, sizeof sys_name, "%s", sp.systemName);
    uint32_t nv = 0;
    for (int i = 0; i < 2; i++) vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!XR_OK(pxrEnumerateViewConfigurationViews(xi, xsys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv)) || nv != 2) {
        pxrDestroyInstance(xi);
        xi = XR_NULL_HANDLE;
        return false;
    }
    LOG("[vr] headset: %s, eye images %ux%u recommended (max %ux%u)\n", sys_name, vcv[0].recommendedImageRectWidth,
        vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
    return true;
}

static void xr_start(void) {
    if (xr_state >= 0) return;
    xr_state = 0;
    if (!vr_on()) return;
    if (getenv("LARECOMP_VR_NO_XR") && atoi(getenv("LARECOMP_VR_NO_XR"))) { LOG("[vr] OpenXR off (LARECOMP_VR_NO_XR): stereo on the desktop only\n"); return; }
    if (!load_loader()) { LOG("[vr] no OpenXR loader (libopenxr_loader.dll): stereo on the desktop only\n"); return; }
    xr_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&pxrEnumerateInstanceExtensionProperties);
    xr_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&pxrCreateInstance);
    if (!pxrEnumerateInstanceExtensionProperties || !pxrCreateInstance) return;
    uint32_t n = 0;
    if (!XR_OK(pxrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL))) {
        LOG("[vr] no OpenXR runtime (is Meta Horizon Link / the Oculus app installed and set as the OpenXR runtime?)\n");
        return;
    }
    XrExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep);
    for (uint32_t i = 0; i < n; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    pxrEnumerateInstanceExtensionProperties(NULL, n, &n, ep);
    if (!has_xr_ext(ep, n, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        LOG("[vr] the OpenXR runtime has no %s\n", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        free(ep);
        return;
    }
    const char *want[8];
    uint32_t nw = 0;
    want[nw++] = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    have_refresh = has_xr_ext(ep, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (have_refresh) want[nw++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    free(ep);
    memcpy(inst_want, want, sizeof want);
    inst_nwant = nw;
    // The headset: Link may still be connecting; LARECOMP_VR_WAIT=<seconds> (default 20) to wait for it
    if (!instance_make(getenv("LARECOMP_VR_WAIT") ? atoi(getenv("LARECOMP_VR_WAIT")) : 20)) return;
    uint32_t len = 0;
    if (!XR_OK(pxrGetVulkanInstanceExtensionsKHR(xi, xsys, sizeof inst_ext_buf, &len, inst_ext_buf))) return;
    xr_state = 1;
}

// split a space-separated list in place
static int split_exts(char *buf, const char **out, int cap) {
    int n = 0;
    for (char *p = buf; *p && n < cap;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        out[n++] = p;
        while (*p && *p != ' ') p++;
    }
    return n;
}
static bool in_list(const char **l, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(l[i], s)) return true;
    return false;
}

int vr_xr_instance_exts(const char **have, int nhave, const char **out, int cap) {
    xr_start();
    if (xr_state <= 0) return 0;
    const char *l[32];
    int n = split_exts(inst_ext_buf, l, 32), k = 0;
    for (int i = 0; i < n && k < cap; i++) if (!in_list(have, nhave, l[i])) out[k++] = l[i];
    for (int i = 0; i < k; i++) LOG("[vr] Vulkan instance extension for OpenXR: %s\n", out[i]);
    return k;
}

bool vr_xr_physical_device(VkInstance inst, VkPhysicalDevice *pd) {
    if (xr_state <= 0) return false;
    if (!XR_OK(pxrGetVulkanGraphicsDeviceKHR(xi, xsys, inst, pd))) { xr_state = 0; return false; }
    return true;
}

int vr_xr_device_exts(const char **have, int nhave, const char **out, int cap) {
    if (xr_state <= 0) return 0;
    uint32_t len = 0;
    if (!XR_OK(pxrGetVulkanDeviceExtensionsKHR(xi, xsys, sizeof dev_ext_buf, &len, dev_ext_buf))) { xr_state = 0; return 0; }
    const char *l[32];
    int n = split_exts(dev_ext_buf, l, 32), k = 0;
    for (int i = 0; i < n && k < cap; i++) if (!in_list(have, nhave, l[i])) out[k++] = l[i];
    for (int i = 0; i < k; i++) LOG("[vr] Vulkan device extension for OpenXR: %s\n", out[i]);
    return k;
}

// ------------------------------------------------------------------ swapchains and our images
typedef struct {
    XrSwapchain sc;
    uint32_t w, h, n;
    VkImage img[8];
    VkCommandBuffer copy[8];  // ours -> img[i] (prerecorded)
    VkImage own;              // ours: drawn on T (UNORM view of the swapchain's format)
    VmaAllocation own_mem;
    VkImageView own_view;
} Chain;
enum { CH_LEFT, CH_RIGHT, CH_PANEL, CH_N };
static Chain ch[CH_N];
static VkFormat sc_fmt, own_fmt;
static VkCommandPool r_pool;  // (R's copies: recorded once on T before R uses them)

static VkFormat unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static bool chain_make(Chain *c, uint32_t w, uint32_t h) {
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = sc_fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XR_OK(pxrCreateSwapchain(xs, &ci, &c->sc))) return false;
    c->w = w, c->h = h;
    uint32_t n = 0;
    pxrEnumerateSwapchainImages(c->sc, 0, &n, NULL);
    if (n > 8) n = 8;
    XrSwapchainImageVulkanKHR im[8];
    for (uint32_t i = 0; i < n; i++) im[i] = (XrSwapchainImageVulkanKHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (!XR_OK(pxrEnumerateSwapchainImages(c->sc, n, &n, (XrSwapchainImageBaseHeader *)im))) return false;
    c->n = n;
    for (uint32_t i = 0; i < n; i++) c->img[i] = im[i].image;
    // our image
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = own_fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ii, &ai, &c->own, &c->own_mem, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->own, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = own_fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &c->own_view) != VK_SUCCESS) return false;
    // the copies: ours (GENERAL) -> the swapchain's image (-> COLOR_ATTACHMENT_OPTIMAL, as the runtime expects)
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r_pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = n};
    if (vkAllocateCommandBuffers(g_vk.dev, &cai, c->copy) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBuffer cb = c->copy[i];
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier2 b[2] = {
            {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->own,
             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
            {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             .srcAccessMask = 0, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
             .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->img[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
        vkCmdPipelineBarrier2(cb, &di);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {w, h, 1}};
        vkCmdCopyImage(cb, c->own, VK_IMAGE_LAYOUT_GENERAL, c->img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        VkImageMemoryBarrier2 a = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->img[i],
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo da = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &a};
        vkCmdPipelineBarrier2(cb, &da);
        vkEndCommandBuffer(cb);
    }
    return true;
}

// ------------------------------------------------------------------ settings
static float set_f(const char *env, float def) { const char *e = getenv(env); return e && *e ? (float)atof(e) : def; }
VrSettings g_vrs;
void vr_settings_init(void) {
    static int done;
    if (done++) return;
    g_vrs.world_scale = set_f("LARECOMP_VR_SCALE", 10.0f);   // game units per meter
    g_vrs.level = set_f("LARECOMP_VR_LEVEL", 1) != 0;          // the game camera's pitch and roll removed
    g_vrs.res = set_f("LARECOMP_VR_RES", 1.0f);               // eye image size x the headset's recommended
    g_vrs.panel_dist = set_f("LARECOMP_VR_PANEL_DIST", 2.2f); // menus: the screen's distance (m)
    g_vrs.panel_width = set_f("LARECOMP_VR_PANEL_WIDTH", 3.0f);
    g_vrs.hud_dist = set_f("LARECOMP_VR_HUD_DIST", 1.6f);     // fights: the HUD panel's distance and width (m)
    g_vrs.hud_width = set_f("LARECOMP_VR_HUD_WIDTH", 0);       // 0: the game camera's view (lines up); else a fixed width (m)
    g_vrs.blink = set_f("LARECOMP_VR_BLINK", 0.2f);
    g_vrs.vignette = set_f("LARECOMP_VR_VIGNETTE", 0.7f);   // comfort vignette while the game camera turns / moves fast         // seconds of black when switching big screen <-> arena
    g_vrs.hz = set_f("LARECOMP_VR_HZ", 0);                    // headset refresh rate request (0: as set)
}

// ------------------------------------------------------------------ session
static bool session_failed;
static void session_make(void) {
    if (xs || session_failed || xr_state <= 0) return;
    session_failed = true;  // (until it succeeds)
    vr_settings_init();
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!XR_OK(pxrGetVulkanGraphicsRequirementsKHR(xi, xsys, &req))) return;
    rec_drain();  // (the runtime may use the queue while it makes the session and its images)
    XrGraphicsBindingVulkanKHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = g_vk.inst;
    gb.physicalDevice = g_vk.phys;
    gb.device = g_vk.dev;
    gb.queueFamilyIndex = g_vk.qfam;
    gb.queueIndex = 0;
    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO, &gb, 0, xsys};
    if (!XR_OK(pxrCreateSession(xi, &si, &xs))) { xs = XR_NULL_HANDLE; return; }
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XR_OK(pxrCreateReferenceSpace(xs, &rs, &space_local))) return;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XR_OK(pxrCreateReferenceSpace(xs, &rs, &space_view))) return;
    int64_t fm[64];
    uint32_t nf = 0;
    pxrEnumerateSwapchainFormats(xs, 64, &nf, fm);
    sc_fmt = VK_FORMAT_UNDEFINED;
    static const VkFormat pref[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (int k = 0; k < 4 && !sc_fmt; k++)
        for (uint32_t i = 0; i < nf; i++) if (fm[i] == pref[k]) { sc_fmt = pref[k]; break; }
    if (!sc_fmt) { LOG("[vr] no 8-bit RGBA swapchain format\n"); return; }
    own_fmt = unorm_of(sc_fmt);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
    if (vkCreateCommandPool(g_vk.dev, &pi, NULL, &r_pool) != VK_SUCCESS) return;
    uint32_t ew = (uint32_t)(vcv[0].recommendedImageRectWidth * g_vrs.res), eh = (uint32_t)(vcv[0].recommendedImageRectHeight * g_vrs.res);
    if (ew > vcv[0].maxImageRectWidth) ew = vcv[0].maxImageRectWidth;
    if (eh > vcv[0].maxImageRectHeight) eh = vcv[0].maxImageRectHeight;
    if (!chain_make(&ch[CH_LEFT], ew, eh) || !chain_make(&ch[CH_RIGHT], ew, eh) || !chain_make(&ch[CH_PANEL], 1920, 1080)) {
        LOG("[vr] swapchains failed\n");
        return;
    }
    // our images start in GENERAL
    VkCommandBuffer cb = vk_cmd();
    for (int i = 0; i < CH_N; i++) vk_image_init_layout(cb, ch[i].own, VK_IMAGE_ASPECT_COLOR_BIT);
    renderer_submit();
    extern void vr_input_attach(XrInstance, XrSession, PFN_xrGetInstanceProcAddr);
    vr_input_attach(xi, xs, xr_gipa);
    session_failed = false;
    LOG("[vr] session: eye images %ux%u (format %d), panel 1920x1080\n", ew, eh, sc_fmt);
}

static void set_refresh(void) {
    if (!have_refresh || !pxrEnumerateDisplayRefreshRatesFB) return;
    float rates[16];
    uint32_t n = 0;
    if (!XR_OK(pxrEnumerateDisplayRefreshRatesFB(xs, 16, &n, rates))) return;
    char s[160] = "";
    for (uint32_t i = 0; i < n; i++) snprintf(s + strlen(s), sizeof s - strlen(s), " %.0f", rates[i]);
    LOG("[vr] refresh rates:%s\n", s);
    if (g_vrs.hz > 0) XR_OK(pxrRequestDisplayRefreshRateFB(xs, g_vrs.hz));
}

// ---- the exact left-eye view on the PC: Meta's own mirror (Oculus Mirror, part of Link) shows the compositor's final
// picture - timewarped to the head's latest pose, with the panels where the headset shows them - which the game's
// window cannot (its mirror is the picture before that). Opened once when the session begins, if Link has it and it
// isn't already open. LARECOMP_VR_OCULUS_MIRROR=0: not opened.
static void headset_mirror_open(void) {
#ifdef _WIN32
    static bool done;
    const char *e = getenv("LARECOMP_VR_OCULUS_MIRROR");
    if (done || (e && !atoi(e))) return;
    done = true;
    if (FindWindowA(NULL, "Oculus Mirror")) return;
    char exe[MAX_PATH];
    const char *pf = getenv("ProgramFiles");
    snprintf(exe, sizeof exe, "%s\\Meta Horizon\\Support\\oculus-diagnostics\\OculusMirror.exe", pf ? pf : "C:\\Program Files");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        snprintf(exe, sizeof exe, "%s\\Oculus\\Support\\oculus-diagnostics\\OculusMirror.exe", pf ? pf : "C:\\Program Files");
        if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return;
    }
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof cmd, "\"%s\" --LeftEyeOnly --Size 1080 1080", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        LOG("[vr] opened the Oculus Mirror window (the headset's exact left-eye view)\n");
    }
#endif
}

// ---- the headset lost (Link dropped, the session closed from the Meta menu): everything OpenXR made goes, with our
// images; vr_xr_frame_next tries again every two seconds, quietly, and the game goes back into the headset.
static bool lost;
static int lost_wait;
static TexBackend mb_eye, mb_hud, mb_out;  // (the mirror's views of our images: forgotten with them)
static void session_teardown(void) {
    rec_drain();
    vkQueueWaitIdle(g_vk.queue);  // (the recorder is idle: the queue is ours a moment)
    extern void vk_views_forget(TexBackend *b);
    vk_views_forget(&mb_eye);
    vk_views_forget(&mb_hud);
    for (int i = 0; i < CH_N; i++) {
        Chain *c = &ch[i];
        if (c->n && r_pool) vkFreeCommandBuffers(g_vk.dev, r_pool, c->n, c->copy);
        if (c->own_view) vkDestroyImageView(g_vk.dev, c->own_view, NULL);
        if (c->own) vmaDestroyImage(g_vk.vma, c->own, c->own_mem);
        memset(c, 0, sizeof *c);
    }
    if (r_pool) vkDestroyCommandPool(g_vk.dev, r_pool, NULL);
    r_pool = VK_NULL_HANDLE;
    if (xi) pxrDestroyInstance(xi);  // (its session, swapchains, spaces and actions go with it)
    xi = XR_NULL_HANDLE, xs = XR_NULL_HANDLE, space_local = space_view = XR_NULL_HANDLE;
    running = false;
    xstate = XR_SESSION_STATE_UNKNOWN;
    session_failed = false;
    extern void vr_input_reset(void);
    vr_input_reset();
    lost = true;
    lost_wait = 0;
}

static void poll_events(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (pxrPollEvent(xi, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *e = (XrEventDataSessionStateChanged *)&ev;
            xstate = e->state;
            LOG("[vr] session state %d\n", xstate);
            if (xstate == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (XR_OK(pxrBeginSession(xs, &bi))) { running = true; set_refresh(); headset_mirror_open(); }
            } else if (xstate == XR_SESSION_STATE_STOPPING && running) {
                rec_drain();
                XR_OK(pxrEndSession(xs));
                running = false;
            } else if (xstate == XR_SESSION_STATE_EXITING || xstate == XR_SESSION_STATE_LOSS_PENDING) {
                LOG("[vr] the headset session ended (%s): the desktop until it is back\n",
                    xstate == XR_SESSION_STATE_EXITING ? "closed" : "lost: Link dropped?");
                session_teardown();
                return;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOG("[vr] the OpenXR runtime is going away: the desktop until it is back\n");
            session_teardown();
            return;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            extern void vr_input_profile_changed(void);
            vr_input_profile_changed();
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            LOG("[vr] recentered\n");
        }
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ------------------------------------------------------------------ the frame
// The frame being translated: its display time and eyes (T only); a frame's copy goes to R with the end packet.
typedef struct {
    bool waited, render;       // xrWaitFrame returned; the runtime wants it rendered
    XrTime time;
    XrView view[2];            // the eyes in LOCAL space
    bool views_ok;
} XrFrame;
static XrFrame cur;
static bool fake_views;  // (LARECOMP_VR_FAKE_HEAD: eyes made up, no headset; tests on the desktop)
static XrView fake[2];

static void fake_init(void) {
    static int done;
    if (done++) return;
    const char *e = getenv("LARECOMP_VR_FAKE_HEAD");  // yaw,pitch[,half fov] degrees
    if (!e) return;
    float yaw = 0, pitch = 0, hf = 50;
    sscanf(e, "%f,%f,%f", &yaw, &pitch, &hf);
    float cy = cosf(yaw * 0.5f * 3.14159265f / 180), sy = sinf(yaw * 0.5f * 3.14159265f / 180);
    float cp = cosf(pitch * 0.5f * 3.14159265f / 180), sp = sinf(pitch * 0.5f * 3.14159265f / 180);
    XrQuaternionf q = {cy * sp, sy * cp, -sy * sp, cy * cp};  // yaw (y) then pitch (x)
    for (int i = 0; i < 2; i++) {
        fake[i] = (XrView){XR_TYPE_VIEW};
        fake[i].pose.orientation = q;
        float ex = (i ? 0.5f : -0.5f) * 0.064f;  // (along the head's x)
        float y2 = q.y * 2, z2 = q.z * 2;
        fake[i].pose.position.x = ex * (1 - (q.y * y2 + q.z * z2));
        fake[i].pose.position.y = ex * (q.x * y2 + q.w * z2);
        fake[i].pose.position.z = ex * (q.x * z2 - q.w * y2);
        float a = hf * 3.14159265f / 180;
        fake[i].fov = (XrFovf){-a, a, a * 0.96f, -a * 0.96f};
    }
    fake_views = true;
    LOG("[vr] fake head: yaw %.0f pitch %.0f, fov +-%.0f degrees\n", yaw, pitch, hf);
}

// ---- the soft recenter: at each switch between the big screen and the arena (and at the start), the head's place and
// heading then become the reference - the fight starts in front of the player, the screens appear where they look.
// The eyes go to the camera relative to it; the panels are placed by it. LARECOMP_VR_AUTO_RECENTER=0: LOCAL as is.
static XrPosef ref = {{0, 0, 0, 1}, {0, 0, 0}};
static bool recenter_req = true;
void vr_xr_recenter(void) { recenter_req = true; }
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {  // v rotated by q
    float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return (XrVector3f){v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
                        v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                           a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static void recenter_take(const XrPosef *head) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_VR_AUTO_RECENTER") || atoi(getenv("LARECOMP_VR_AUTO_RECENTER"));
    if (!recenter_req || !on) return;
    recenter_req = false;
    XrVector3f f = qrot(head->orientation, (XrVector3f){0, 0, -1});
    float yaw = atan2f(-f.x, -f.z);
    ref.orientation = (XrQuaternionf){0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    ref.position = head->position;
    LOG("[vr] recentered: heading %.0f degrees, head at %.2f %.2f %.2f\n", yaw * 57.29578f, head->position.x,
        head->position.y, head->position.z);
}

static uint32_t view_gen;  // bumped when the eyes change (exec.c memoizes the patched camera per eye and generation)
uint32_t vr_view_gen(void) { return view_gen; }

void vr_xr_frame_next(void) {
    fake_init();
    view_gen++;
    if (lost) {  // (every ~2 s: the headset back?)
        cur.waited = cur.views_ok = false;
        if (++lost_wait < 120) return;
        lost_wait = 0;
        if (!instance_make(0)) return;
        lost = false;
        LOG("[vr] the headset is back: reconnecting\n");
        recenter_req = true;
    }
    if (xr_state <= 0) return;
    session_make();
    if (!xs) return;
    poll_events();
    cur.waited = false;
    cur.views_ok = false;
    if (!running) return;
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (!XR_OK(pxrWaitFrame(xs, NULL, &fs))) return;
    cur.waited = true;
    cur.render = fs.shouldRender;
    cur.time = fs.predictedDisplayTime;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, fs.predictedDisplayTime, space_local};
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    cur.view[0] = cur.view[1] = (XrView){XR_TYPE_VIEW};
    if (XR_OK(pxrLocateViews(xs, &li, &vs, 2, &nv, cur.view)) && nv == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        cur.views_ok = true;
    extern void vr_input_sync(XrSession, XrSpace, XrTime, const XrPosef *head);
    XrPosef head = cur.view[0].pose;  // (the head: between the eyes)
    head.position.x = (cur.view[0].pose.position.x + cur.view[1].pose.position.x) * 0.5f;
    head.position.y = (cur.view[0].pose.position.y + cur.view[1].pose.position.y) * 0.5f;
    head.position.z = (cur.view[0].pose.position.z + cur.view[1].pose.position.z) * 0.5f;
    if (cur.views_ok) recenter_take(&head);
    vr_input_sync(xs, space_local, fs.predictedDisplayTime, cur.views_ok ? &head : NULL);
}

// The eye's pose (LOCAL space: x right, y up, z back, meters; the head's place at the recenter is the origin) and its
// field of view (tangents: left, right, up, down - left and down negative) for the frame being translated.
bool vr_eye_view(int eye, float q[4], float p[3], float tan[4]) {
    const XrView *v = fake_views ? &fake[eye] : cur.views_ok ? &cur.view[eye] : NULL;
    if (!v) return false;
    XrPosef e = v->pose;
    if (!fake_views) {  // (relative to the soft recenter's reference)
        XrQuaternionf inv = {-ref.orientation.x, -ref.orientation.y, -ref.orientation.z, ref.orientation.w};
        XrVector3f d = {e.position.x - ref.position.x, e.position.y - ref.position.y, e.position.z - ref.position.z};
        e.position = qrot(inv, d);
        e.orientation = qmul(inv, e.orientation);
    }
    q[0] = e.orientation.x, q[1] = e.orientation.y, q[2] = e.orientation.z, q[3] = e.orientation.w;
    p[0] = e.position.x, p[1] = e.position.y, p[2] = e.position.z;
    tan[0] = tanf(v->fov.angleLeft), tan[1] = tanf(v->fov.angleRight), tan[2] = tanf(v->fov.angleUp), tan[3] = tanf(v->fov.angleDown);
    return true;
}

// ------------------------------------------------------------------ the end of a frame (R)
typedef struct {
    XrTime time;
    bool render, immersive, panel, blank;
    XrView view[2];
    XrPosef panel_pose;
    float panel_w, panel_h;
} EndArgs;
static _Atomic uint64_t frames_ended;
static uint64_t frames_queued;

static bool chain_put(Chain *c) {
    uint32_t i;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_OK(pxrAcquireSwapchainImage(c->sc, &ai, &i))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
    if (!XR_OK(pxrWaitSwapchainImage(c->sc, &wi))) return false;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->copy[i]};
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_OK(pxrReleaseSwapchainImage(c->sc, &ri));
}

static void end_run(const void *p) {
    const EndArgs *a = p;
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (!XR_OK(pxrBeginFrame(xs, &bi))) { atomic_fetch_add(&frames_ended, 1); return; }
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nl = 0;
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (a->render && !a->blank) {
        if (a->immersive && chain_put(&ch[CH_LEFT]) && chain_put(&ch[CH_RIGHT])) {
            for (int e = 0; e < 2; e++) {
                pv[e] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[e].pose = a->view[e].pose;
                pv[e].fov = a->view[e].fov;
                pv[e].subImage.swapchain = ch[e].sc;
                pv[e].subImage.imageRect.extent.width = (int32_t)ch[e].w;
                pv[e].subImage.imageRect.extent.height = (int32_t)ch[e].h;
            }
            proj.space = space_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        if (a->panel && chain_put(&ch[CH_PANEL])) {
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            quad.space = space_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = ch[CH_PANEL].sc;
            quad.subImage.imageRect.extent.width = (int32_t)ch[CH_PANEL].w;
            quad.subImage.imageRect.extent.height = (int32_t)ch[CH_PANEL].h;
            quad.pose = a->panel_pose;
            quad.size.width = a->panel_w;
            quad.size.height = a->panel_h;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
        }
    }
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = a->time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl;
    ei.layers = layers;
    XR_OK(pxrEndFrame(xs, &ei));
    atomic_fetch_add(&frames_ended, 1);
}

// The panel (menus: the whole picture; fights: the HUD) faces the user, level, at the head's height at the recenter.
static XrPosef panel_pose(float dist) {  // (dist m in front of the reference, facing it)
    XrVector3f o = qrot(ref.orientation, (XrVector3f){0, 0, -dist});
    XrPosef p = {ref.orientation, {ref.position.x + o.x, ref.position.y + o.y, ref.position.z + o.z}};
    return p;
}

// The HUD panel. By default it covers exactly what the flat game's camera sees (its field of view, its pitch, its
// height relative to the player), as if the TV picture's HUD were drawn on a window in front of that camera: what the
// game places on the screen over something in the world (damage numbers, markers, names) lines up with it.
// LARECOMP_VR_HUD_WIDTH=<m>: a fixed panel that size instead (smaller: easier to read, no longer lined up).
static void hud_place(XrPosef *pose, float *w, float *h) {
    float d = g_vrs.hud_dist > 0.3f ? g_vrs.hud_dist : 1.6f;
    extern bool vr_camera_is_game(void);
    if (g_vrs.hud_width > 0 || !vr_camera_is_game()) {  // (a fixed panel: or the game camera isn't where the player is)
        if (g_vrs.hud_width <= 0) {
            *pose = panel_pose(d);
            *w = 1.8f, *h = 1.8f * 9 / 16;
            return;
        }
        *pose = panel_pose(d);
        *w = g_vrs.hud_width, *h = g_vrs.hud_width * 9 / 16;
        return;
    }
    extern void vr_game_view(float *fovy, float *pitch, float *dy);
    float fovy, pitch, dy;
    vr_game_view(&fovy, &pitch, &dy);
    XrQuaternionf qp = {sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f)};
    XrVector3f f = qrot(qp, (XrVector3f){0, 0, -d});
    XrVector3f o = qrot(ref.orientation, (XrVector3f){f.x, f.y + dy, f.z});
    pose->orientation = qmul(ref.orientation, qp);
    pose->position = (XrVector3f){ref.position.x + o.x, ref.position.y + o.y, ref.position.z + o.z};
    *h = 2 * d * tanf(fovy * 0.5f);
    *w = *h * 16 / 9;
}

// ---- the desktop's mirror: what the left eye sees (the HUD panel drawn where it appears), letterboxed in the window
static Chain mirror;  // (only its own image)
static bool mirror_ready;
static TexBackend wrap(VkImage img, uint32_t w, uint32_t h) {
    return (TexBackend){.image = img, .format = VK_FORMAT_R8G8B8A8_UNORM, .width = w, .height = h, .depth = 1, .layers = 1,
                        .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .sx = 1, .sy = 1, .ax = 1, .ay = 1};
}
void vk_vr_mirror(TexBackend *eye, TexBackend *hud, TexBackend *dst, const float rect[4], bool dark);
static void mirror_make(const void *end_args);
TexBackend *vr_mirror_take(void) {
    bool r = mirror_ready;
    mirror_ready = false;
    return r ? &mb_out : NULL;
}

// ------------------------------------------------------------------ present (T)
static bool frame_scene;  // the scene camera was drawn with the eyes' views this frame (vr.c)
void vr_note_scene(void) { frame_scene = true; }

void vk_vr_blit(TexBackend *b, VkImageView dst, uint32_t w, uint32_t h, const float crop[4], bool screen_filter);
void vk_vr_eye(TexBackend *b, VkImageView dst_view, uint32_t w, uint32_t h, const float crop[4], float vignette, float cx);
void vk_vr_hud_combine(TexBackend *a, TexBackend *b, VkImage dst_img, VkImageView dst_view, uint32_t w, uint32_t h, const float crop[4]);

// (LARECOMP_VR_FAKE_HEAD without a headset) our images alone, at a Quest 3's eye size: the frame's pictures are made
// as for the headset (LARECOMP_VR_SHOT_AT saves them), nothing is sent
static bool offline_make(void) {
    static int done;
    if (done) return done > 0;
    done = -1;
    own_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t sz[CH_N][2] = {{1680, 1760}, {1680, 1760}, {1920, 1080}};
    for (int i = 0; i < CH_N; i++) {
        Chain *c = &ch[i];
        c->w = sz[i][0], c->h = sz[i][1];
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = own_fmt,
                                .extent = {c->w, c->h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ii, &ai, &c->own, &c->own_mem, NULL) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->own, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = own_fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &c->own_view) != VK_SUCCESS) return false;
        vk_image_init_layout(vk_cmd(), c->own, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    done = 1;
    LOG("[vr] offline: the headset's pictures made without one (LARECOMP_VR_SHOT_AT saves them)\n");
    return true;
}

void vr_xr_present(NvnTexture *t, const float crop[4]) {
    bool scene = frame_scene;
    frame_scene = false;
    bool offline = !xs && fake_views;
    if (offline ? !offline_make() : !cur.waited || !xs) {
        TexBackend *ha, *hb;
        vr_hud_take(&ha, &hb);  // (the next frame starts over)
        return;
    }
    cur.waited = false;
    if (offline) cur.render = true, cur.views_ok = true, cur.view[0] = fake[0], cur.view[1] = fake[1];
    TexBackend *b = t ? t->backend : NULL;
    // (the previous frame's end must have run before this one begins: R is far ahead of here, this never waits long)
    while (atomic_load(&frames_ended) < frames_queued) {
#ifdef _WIN32
        Sleep(0);
#endif
    }
    EndArgs a = {.time = cur.time, .render = cur.render && b};
    static TexBackend *shot_ha, *shot_hb;  // (LARECOMP_VR_SHOT_AT) this frame's HUD images
    if (a.render) {
        a.immersive = scene && cur.views_ok;
        {   // switching between the big screen and the arena: a short blink (black) instead of a cut
            static int last = -1, blink;
            extern int g_vr_blink_req;  // (vr.c: a camera mode change, a snap turn)
            if ((last >= 0 && last != (int)a.immersive) || g_vr_blink_req) blink = (int)(g_vrs.blink * 60 + 0.5f), g_vr_blink_req = 0;
            last = a.immersive;
            if (blink > 0) blink--, a.blank = true;
        }
        if (a.immersive) {
            a.view[0] = cur.view[0], a.view[1] = cur.view[1];
            for (int e = 0; e < 2; e++) {
                TexBackend *src = e && b->vr_twin ? b->vr_twin : b;
                if (own_fmt == VK_FORMAT_R8G8B8A8_UNORM) {
                    float tl = tanf(cur.view[e].fov.angleLeft), tr = tanf(cur.view[e].fov.angleRight);
                    float cx = tr - tl > 1e-4f ? -(tr + tl) / (tr - tl) * 0.5f : 0;  // (the eye's optical centre, uv)
                    vk_vr_eye(src, ch[e].own_view, ch[e].w, ch[e].h, crop, vr_vignette() * g_vrs.vignette, cx);
                } else vk_vr_blit(src, ch[e].own_view, ch[e].w, ch[e].h, crop, true);
            }
            TexBackend *ha, *hb;
            shot_ha = shot_hb = NULL;
            if (vr_hud_take(&ha, &hb) && own_fmt == VK_FORMAT_R8G8B8A8_UNORM) {
                shot_ha = ha, shot_hb = hb;
                static const float whole[4] = {0, 0, 1, 1};
                vk_vr_hud_combine(ha, hb, ch[CH_PANEL].own, ch[CH_PANEL].own_view, ch[CH_PANEL].w, ch[CH_PANEL].h,
                                  vr_hud_on_window() ? crop : whole);
                a.panel = true;
                hud_place(&a.panel_pose, &a.panel_w, &a.panel_h);
            }
            mirror_make(&a);
        } else {  // a menu, a loading screen, a cutscene: the picture on a big screen
            TexBackend *ha, *hb;
            vr_hud_take(&ha, &hb);
            vk_vr_blit(b, ch[CH_PANEL].own_view, ch[CH_PANEL].w, ch[CH_PANEL].h, crop, true);
            a.panel = true;
            a.panel_pose = panel_pose(g_vrs.panel_dist);
            a.panel_w = g_vrs.panel_width, a.panel_h = g_vrs.panel_width * 9 / 16;
            mirror_make(&a);  // (the screen as the left eye sees it, in the dark)
        }
        renderer_submit();
        // LARECOMP_VR_SHOT_AT=<present>,...: what goes to the headset that frame -> build/frames/vr_N_{L,R,panel}.png
        // (also while playing: creating build/vrtest/shot.req saves the next frame's, then the file goes)
        static const char *shots;
        static int sinit, req_tick;
        if (!sinit) sinit = 1, shots = getenv("LARECOMP_VR_SHOT_AT");
        bool req = false;
        if (++req_tick >= 30) {  // (twice a second)
            req_tick = 0;
            FILE *rf = fopen("build/vrtest/shot.req", "rb");
            if (rf) fclose(rf), remove("build/vrtest/shot.req"), req = true;
        }
        if (shots || req) {
            uint64_t n = vr_frame_id();  // (the frame being presented)
            static const char none[] = "";
            for (const char *s = shots ? shots : none; req || *s;) {
                char *e = (char *)s;
                unsigned long long at = req ? n : strtoull(s, &e, 10);
                if (req) e = (char *)none;
                req = false;
                if (at == n) {
                    void renderer_save_texture_tex(TexBackend *b, const char *path);
                    static const char *nm[CH_N] = {"L", "R", "panel"};
                    for (int i = 0; i < CH_N; i++) {
                        if (i < CH_PANEL ? !a.immersive : !a.panel) continue;
                        static TexBackend sb[CH_N];
                        sb[i] = (TexBackend){.image = ch[i].own, .format = own_fmt, .width = ch[i].w, .height = ch[i].h,
                                             .depth = 1, .layers = 1, .levels = 1, .samples = 1,
                                             .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .sx = 1, .sy = 1, .ax = 1, .ay = 1};
                        char path[128];
                        snprintf(path, sizeof path, "build/frames/vr_%05llu_%s.png", at, nm[i]);
                        renderer_save_texture_tex(&sb[i], path);
                        LOG("[vr] saved %s\n", path);
                    }
                    for (int i = 0; i < 2 && shot_ha; i++) {  // (the HUD over black / white: its coverage, offline)
                        char path[128];
                        snprintf(path, sizeof path, "build/frames/vr_%05llu_hud_%s.png", at, i ? "white" : "black");
                        renderer_save_texture_tex(i ? shot_hb : shot_ha, path);
                    }
                }
                if (*e != ',') break;
                s = e + 1;
            }
        }
    }
    if (offline) return;
    frames_queued++;
    rec_call(end_run, &a, sizeof a);
}

void vr_xr_quit(void) {
    if (!xs) return;
    rec_drain();
    if (running) { pxrRequestExitSession(xs); }
}

// (the present, after the eyes and the HUD panel are drawn) the mirror from them
static void mirror_make(const void *end_args) {
    static int on = -1;  // (only for the game's window, when asked: LARECOMP_VR_DESKTOP_MIRROR=1)
    if (on < 0) on = getenv("LARECOMP_VR_DESKTOP_MIRROR") && atoi(getenv("LARECOMP_VR_DESKTOP_MIRROR"));
    if (!on) return;
    const EndArgs *a = end_args;  // (immersive: the left eye's picture with the HUD panel; else the big screen in the dark)
    if (own_fmt != VK_FORMAT_R8G8B8A8_UNORM || !ch[CH_LEFT].own) return;
    if (!mirror.own) {
        mirror.w = ch[CH_LEFT].w, mirror.h = ch[CH_LEFT].h;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = own_fmt,
                                .extent = {mirror.w, mirror.h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ii, &ai, &mirror.own, &mirror.own_mem, NULL) != VK_SUCCESS) { mirror.own = VK_NULL_HANDLE; return; }
        vk_image_init_layout(vk_cmd(), mirror.own, VK_IMAGE_ASPECT_COLOR_BIT);
        LOG("[vr] desktop mirror: the left eye, %ux%u\n", mirror.w, mirror.h);
    }
    mb_eye = wrap(ch[CH_LEFT].own, ch[CH_LEFT].w, ch[CH_LEFT].h);
    mb_hud = wrap(ch[CH_PANEL].own, ch[CH_PANEL].w, ch[CH_PANEL].h);
    mb_out = wrap(mirror.own, mirror.w, mirror.h);
    float rect[4] = {0, 0, 0, 0};
    bool hud = false;
    if (a->panel && (cur.views_ok || fake_views)) {  // the panel's corners seen from the left eye
        const XrView *v = &cur.view[0];
        XrQuaternionf inv = {-v->pose.orientation.x, -v->pose.orientation.y, -v->pose.orientation.z, v->pose.orientation.w};
        float tl = tanf(v->fov.angleLeft), tr = tanf(v->fov.angleRight), tu = tanf(v->fov.angleUp), td = tanf(v->fov.angleDown);
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        hud = true;
        for (int k = 0; k < 4 && hud; k++) {
            XrVector3f c = qrot(a->panel_pose.orientation, (XrVector3f){(k & 1 ? 0.5f : -0.5f) * a->panel_w, (k & 2 ? 0.5f : -0.5f) * a->panel_h, 0});
            c.x += a->panel_pose.position.x - v->pose.position.x;
            c.y += a->panel_pose.position.y - v->pose.position.y;
            c.z += a->panel_pose.position.z - v->pose.position.z;
            XrVector3f d = qrot(inv, c);
            if (d.z > -0.01f) { hud = false; break; }  // (behind the eye)
            float x = d.x / -d.z, y = d.y / -d.z;
            float u = (x - tl) / (tr - tl), w = (tu - y) / (tu - td);
            u0 = fminf(u0, u), u1 = fmaxf(u1, u), v0 = fminf(v0, w), v1 = fmaxf(v1, w);
        }
        rect[0] = u0, rect[1] = v0, rect[2] = u1, rect[3] = v1;
    }
    if (!a->immersive && !hud) return;  // (nothing to show: the desktop shows the game's picture)
    vk_vr_mirror(&mb_eye, hud ? &mb_hud : NULL, &mb_out, rect, !a->immersive);
    mirror_ready = true;
}
