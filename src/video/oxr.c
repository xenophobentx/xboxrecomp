/*
 * oxr.c - a VR headset through OpenXR; see oxr.h. Written against OpenXR 1.1 (the Khronos headers, Apache-2.0 or MIT),
 * the loader opened at run time.
 *
 * Two graphics back ends to the runtime, chosen at start-up:
 *
 *   Direct3D 11 (the default on Windows; the VR page or RECOMP_VR_API=vulkan chooses the other). SteamVR's OpenXR runtime crashes in
 *   xrCreateSwapchain for any Vulkan session on the development PC (an access violation inside the runtime, whatever the
 *   swapchain's shape, the way the Vulkan device was made, the Vulkan version, the layers: tools/win/xr-probe.c
 *   reproduces it on its own), while the same call through Direct3D 11 works. So the renderer stays Vulkan, and each
 *   stereo frame goes to the headset through a Direct3D 11 texture both APIs share: the renderer's frame is copied
 *   into a shared texture (a Vulkan image on the memory of a D3D11 texture, VK_KHR_external_memory_win32), the copy is
 *   waited for on the CPU, and the headset's D3D11 swapchain image is filled by CopyResource from it. The Vulkan device
 *   is the renderer's own (made normally, on the GPU the runtime names by LUID, with the external memory extensions).
 *
 *   Vulkan (XR_KHR_vulkan_enable2): the Vulkan instance, physical device and device are made through the runtime
 *   (creation hooks in nv2a_vk_core.h) and the frame is copied raw into the runtime's Vulkan swapchain. Works with a
 *   runtime that has no such bug.
 *
 * Both: the session is made at the first stereo frame; the swapchain is the stereo frame's size, an sRGB format (the
 * game's frame is raw display bytes, copied raw: the compositor then decodes it as it is meant); each frame the left half
 * and the right half are the two eyes' sub-images of one projection layer in the VIEW space (head-locked for now), whose
 * field of view is the game camera's view window (from the stereo value). The queue is shared with the renderer's
 * submits: everything that submits to it, or that the runtime may use it for, holds nvk_queue_lock().
 */
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include "oxr.h"
#include "recomp_env.h"

#if defined(XBOXRECOMP_HAVE_OPENXR) && defined(XBOXRECOMP_HAVE_VULKAN)

#include <math.h>
#include <stdio.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan_win32.h>      /* the build force-includes vulkan.h before this file defines the platform */
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define OXR_HAVE_D3D11 1
#else
#include <dlfcn.h>
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../d3d/nv2a_vk_core.h"
#include "../kernel/nv2a_pb_state.h"

#define VRLOG "[VR] "

#define XR_FUNCS_COMMON(F) \
    F(xrDestroyInstance) F(xrGetSystem) F(xrGetSystemProperties) F(xrPollEvent) \
    F(xrCreateSession) F(xrDestroySession) F(xrBeginSession) F(xrEndSession) F(xrCreateReferenceSpace) \
    F(xrDestroySpace) F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrDestroySwapchain) \
    F(xrEnumerateSwapchainImages) F(xrAcquireSwapchainImage) F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage) \
    F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame) F(xrResultToString) F(xrLocateViews)
#define XR_FUNCS_VK(F) \
    F(xrGetVulkanGraphicsRequirements2KHR) F(xrCreateVulkanInstanceKHR) F(xrGetVulkanGraphicsDevice2KHR) \
    F(xrCreateVulkanDeviceKHR)
#if defined(OXR_HAVE_D3D11)
#define XR_FUNCS_D3D(F) F(xrGetD3D11GraphicsRequirementsKHR)
#else
#define XR_FUNCS_D3D(F)
#endif
#define XR_DECL(n) static PFN_##n p_##n;
XR_FUNCS_COMMON(XR_DECL)
XR_FUNCS_VK(XR_DECL)
XR_FUNCS_D3D(XR_DECL)
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
static PFN_xrCreateInstance p_xrCreateInstance;

static struct {
    int inited, ok, failed, running, session_made, ended;
    int d3d;                                    /* the Direct3D 11 back end (else Vulkan) */
    void *lib;
    XrInstance inst;
    XrSystemId sys;
    XrSession sess;
    XrSpace space;                              /* LOCAL: the head and the eyes are tracked in it */
    XrSwapchain sc;
    uint32_t nimg, sw, sh;
    int64_t sfmt;                               /* the swapchain format: a VkFormat or a DXGI_FORMAT */
    void *imgs;                                 /* the runtime's images (XrSwapchainImageVulkan2KHR or ...D3D11KHR) */
    VkCommandPool pool;
    VkCommandBuffer *cbs;
    VkFence *fences;
    char sysname[XR_MAX_SYSTEM_NAME_SIZE];
    unsigned frames;
    int swap;                                   /* RECOMP_VR_SWAP=1: the left half to the right eye and the other way */
#if defined(OXR_HAVE_D3D11)
    LUID luid;                                  /* the GPU the runtime wants */
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *shared;                    /* the texture both APIs see */
    VkImage vkimg;                              /* ... as a Vulkan image */
    VkDeviceMemory vkmem;
    int vkimg_ready;
#endif
} X;

/* The head poses: the latest, for the game, and a ring of the recent ones by id, for the frames drawn from them. */
static struct { xbox_vr_pose pose; } s_ring[256];
static xbox_vr_pose s_latest;
static volatile unsigned s_pose_seq;            /* a seqlock: odd while s_latest is being written */
static unsigned s_next_id = 1;
static int s_menu_set;                          /* the menu screen has its place */
static float s_menu_pose[7];                    /* its orientation (x y z w) and position */
void xbox_VrRecentre(void) { s_menu_set = 0; }
void xbox_VrFrame(unsigned id, float vwx, float vwy)
{
    if (id && id < 256 && s_ring[id].pose.id == id && vwx > 0.1f && vwy > 0.1f) {
        s_ring[id].pose.vwx = vwx;
        s_ring[id].pose.vwy = vwy;
    }
}

int xbox_VrPose(xbox_vr_pose *out)
{
    unsigned a, b;

    do {
        a = __atomic_load_n(&s_pose_seq, __ATOMIC_ACQUIRE);
        *out = s_latest;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&s_pose_seq, __ATOMIC_RELAXED);
    } while ((a & 1u) || a != b);
    return a != 0 && out->id != 0;
}

/* The first frames log each step before it runs (stderr is unbuffered): a crash inside the runtime then names its call. */
static unsigned s_step_n;
#define STEP(what) do { if (s_step_n < 60) { s_step_n++; fprintf(stderr, VRLOG "step: %s\n", what); } } while (0)

static const char *rs(XrResult r)
{
    static char b[XR_MAX_RESULT_STRING_SIZE];

    if (X.inst && p_xrResultToString && XR_SUCCEEDED(p_xrResultToString(X.inst, r, b)))
        return b;
    snprintf(b, sizeof b, "XrResult %d", (int)r);
    return b;
}

static void *open_loader(void)
{
#if defined(_WIN32)
    char steamvr[MAX_PATH];
    const char *pf = getenv("ProgramFiles(x86)");
    void *h = (void *)LoadLibraryA("openxr_loader.dll");

    if (!h && pf) {
        snprintf(steamvr, sizeof steamvr, "%s\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll", pf);
        h = (void *)LoadLibraryA(steamvr);
    }
    return h;
#else
    void *h = dlopen("libopenxr_loader.so.1", RTLD_NOW);

    return h ? h : dlopen("libopenxr_loader.so", RTLD_NOW);
#endif
}

static void *sym(const char *name)
{
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)X.lib, name);
#else
    return dlsym(X.lib, name);
#endif
}

/* ---- the Vulkan back end: creation through the runtime ------------------------------------------- */

/* The function lookup the runtime is given. It calls vkCreateInstance and vkCreateDevice itself, with the create info
 * it has added to; those two must be the toolkit's own (vk_dyn.h: the instance's and the device's function pointers
 * are loaded by those wrappers: with the loader's bare ones the next Vulkan call finds a null pointer). */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL oxr_gipa(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (instance != VK_NULL_HANDLE && !strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)vkCreateDevice;
    return vkGetInstanceProcAddr(instance, name);
}

static VkResult hk_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out)
{
    XrVulkanInstanceCreateInfoKHR xi;
    VkResult vr = VK_SUCCESS;
    XrResult r;

    memset(&xi, 0, sizeof xi);
    xi.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    xi.systemId = X.sys;
    xi.pfnGetInstanceProcAddr = oxr_gipa;
    xi.vulkanCreateInfo = ci;
    r = p_xrCreateVulkanInstanceKHR(X.inst, &xi, out, &vr);
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateVulkanInstanceKHR: %s\n", rs(r));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vr == VK_SUCCESS)
        fprintf(stderr, VRLOG "the Vulkan instance was made through the OpenXR runtime\n");
    return vr;
}

static VkPhysicalDevice hk_physical_device_vk(VkInstance inst)
{
    XrVulkanGraphicsDeviceGetInfoKHR gi;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    XrResult r;

    memset(&gi, 0, sizeof gi);
    gi.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    gi.systemId = X.sys;
    gi.vulkanInstance = inst;
    r = p_xrGetVulkanGraphicsDevice2KHR(X.inst, &gi, &pd);
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrGetVulkanGraphicsDevice2KHR: %s\n", rs(r));
        return VK_NULL_HANDLE;
    }
    return pd;
}

static VkResult hk_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out)
{
    XrVulkanDeviceCreateInfoKHR di;
    VkResult vr = VK_SUCCESS;
    XrResult r;

    memset(&di, 0, sizeof di);
    di.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    di.systemId = X.sys;
    di.pfnGetInstanceProcAddr = oxr_gipa;
    di.vulkanPhysicalDevice = pd;
    di.vulkanCreateInfo = ci;
    r = p_xrCreateVulkanDeviceKHR(X.inst, &di, out, &vr);
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateVulkanDeviceKHR: %s\n", rs(r));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vr == VK_SUCCESS)
        fprintf(stderr, VRLOG "the Vulkan device was made through the OpenXR runtime\n");
    return vr;
}

static const struct nvk_create_hooks s_hooks_vk = { hk_create_instance, hk_physical_device_vk, hk_create_device, NULL };

#if defined(OXR_HAVE_D3D11)
/* ---- the Direct3D 11 back end: the renderer's own Vulkan device, on the runtime's GPU ----------------- */

/* The GPU the runtime wants, by LUID among the Vulkan devices. */
static VkPhysicalDevice hk_physical_device_d3d(VkInstance inst)
{
    VkPhysicalDevice pds[8];
    uint32_t n = 8, i;

    if (vkEnumeratePhysicalDevices(inst, &n, pds) != VK_SUCCESS && !n)
        return VK_NULL_HANDLE;
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties idp;
        VkPhysicalDeviceProperties2 p2;

        memset(&idp, 0, sizeof idp);
        idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        memset(&p2, 0, sizeof p2);
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (idp.deviceLUIDValid && !memcmp(idp.deviceLUID, &X.luid, sizeof X.luid)) {
            fprintf(stderr, VRLOG "the headset's GPU (LUID %08lx:%08lx) is \"%s\"\n", (unsigned long)X.luid.HighPart,
                    (unsigned long)X.luid.LowPart, p2.properties.deviceName);
            return pds[i];
        }
    }
    fprintf(stderr, VRLOG "no Vulkan device has the headset's LUID: the renderer's own choice\n");
    return VK_NULL_HANDLE;
}

static const char *const s_dev_exts[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_win32" };

static const char *const *hk_device_extensions(unsigned *n)
{
    *n = (unsigned)(sizeof s_dev_exts / sizeof s_dev_exts[0]);
    return s_dev_exts;
}

static const struct nvk_create_hooks s_hooks_d3d = { NULL, hk_physical_device_d3d, NULL, hk_device_extensions };
#endif

static void wait_one_second(void)
{
#if defined(_WIN32)
    Sleep(1000);
#else
    sleep(1);
#endif
}

int xbox_VrInit(void)
{
    XrInstanceCreateInfo ci;
    XrSystemGetInfo gi;
    XrSystemProperties sp;
    const char *ext;
    const char *api = getenv("RECOMP_VR_API") ? getenv("RECOMP_VR_API") : getenv("RECOMP_VR_API_SAVED");   /* the saved one: main.c reads it from the settings file */
    const char *wait_s = getenv("RECOMP_VR_WAIT");
    XrResult r;
    time_t deadline;
    int waited = 0;

    if (X.inited)
        return X.ok;
    X.inited = 1;
#if defined(OXR_HAVE_D3D11)
    X.d3d = !(api && !strcmp(api, "vulkan"));
    X.swap = getenv("RECOMP_VR_SWAP") != NULL && *getenv("RECOMP_VR_SWAP") == '1';
#else
    X.d3d = 0;
    (void)api;
#endif
    ext = X.d3d ? "XR_KHR_D3D11_enable" : "XR_KHR_vulkan_enable2";
#if defined(_WIN32)
    if (!X.d3d && !getenv("DISABLE_XR_APILAYER_NOVENDOR_motion_compensation")) {
        /* The OpenXR-MotionCompensation layer (an implicit API layer, installed for motion rigs) crashes in xrCreateSwapchain
         * for a Vulkan session (an access violation inside XR_APILAYER_NOVENDOR_motion_compensation.dll, seen on the development
         * PC): off for this process on the Vulkan path (the Direct3D 11 one works with it). Its loader switch is its manifest's
         * disable_environment. */
        _putenv_s("DISABLE_XR_APILAYER_NOVENDOR_motion_compensation", "1");
        fprintf(stderr, VRLOG "the Vulkan path: the OpenXR motion compensation layer is switched off for this run\n");
    }
#endif
    X.lib = open_loader();
    if (!X.lib) {
        fprintf(stderr, VRLOG "no OpenXR loader (openxr_loader.dll): VR off\n");
        return 0;
    }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)sym("xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) {
        fprintf(stderr, VRLOG "the OpenXR loader has no xrGetInstanceProcAddr: VR off\n");
        return 0;
    }
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance);
    if (!p_xrCreateInstance)
        return 0;
    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Takedawn");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "xboxrecomp");
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    deadline = time(NULL) + (wait_s ? atoi(wait_s) : 300);       /* RECOMP_VR_WAIT seconds, default 300 */
    r = p_xrCreateInstance(&ci, &X.inst);
    while (XR_FAILED(r) && time(NULL) < deadline) {                /* SteamVR not up yet: wait for it */
        if (!waited++)
            fprintf(stderr, VRLOG "no OpenXR runtime yet (%d): waiting for it (start SteamVR), up to %s s\n", (int)r,
                    wait_s ? wait_s : "300");
        wait_one_second();
        r = p_xrCreateInstance(&ci, &X.inst);
    }
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateInstance: %d (no OpenXR runtime, or it lacks %s): VR off\n", (int)r, ext);
        return 0;
    }
#define XR_LOAD(n) \
    if (XR_FAILED(p_xrGetInstanceProcAddr(X.inst, #n, (PFN_xrVoidFunction *)&p_##n)) || !p_##n) { \
        fprintf(stderr, VRLOG "the runtime has no %s: VR off\n", #n); \
        return 0; \
    }
    XR_FUNCS_COMMON(XR_LOAD)
    if (X.d3d) {
#if defined(OXR_HAVE_D3D11)
        XR_FUNCS_D3D(XR_LOAD)
#endif
    } else {
        XR_FUNCS_VK(XR_LOAD)
    }
#undef XR_LOAD
    memset(&gi, 0, sizeof gi);
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = p_xrGetSystem(X.inst, &gi, &X.sys);
    while (XR_FAILED(r) && time(NULL) < deadline) {                /* the headset is not ready: wait for it */
        if (!waited++)
            fprintf(stderr, VRLOG "no headset yet (%s): waiting for it (put it on, wake it), up to %s s\n", rs(r),
                    wait_s ? wait_s : "300");
        wait_one_second();
        r = p_xrGetSystem(X.inst, &gi, &X.sys);
    }
    if (waited && XR_SUCCEEDED(r))
        fprintf(stderr, VRLOG "the headset is there\n");
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "no headset: %s (connect it and start the runtime: SteamVR): VR off\n", rs(r));
        return 0;
    }
    memset(&sp, 0, sizeof sp);
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    p_xrGetSystemProperties(X.inst, X.sys, &sp);
    snprintf(X.sysname, sizeof X.sysname, "%s", sp.systemName);
    if (X.d3d) {
#if defined(OXR_HAVE_D3D11)
        XrGraphicsRequirementsD3D11KHR req;

        memset(&req, 0, sizeof req);
        req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
        r = p_xrGetD3D11GraphicsRequirementsKHR(X.inst, X.sys, &req);
        if (XR_FAILED(r)) {
            fprintf(stderr, VRLOG "xrGetD3D11GraphicsRequirementsKHR: %s: VR off\n", rs(r));
            return 0;
        }
        X.luid = req.adapterLuid;
        nvk_set_create_hooks(&s_hooks_d3d);
#endif
    } else {
        XrGraphicsRequirementsVulkan2KHR req;

        memset(&req, 0, sizeof req);
        req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
        r = p_xrGetVulkanGraphicsRequirements2KHR(X.inst, X.sys, &req);
        if (XR_FAILED(r)) {
            fprintf(stderr, VRLOG "xrGetVulkanGraphicsRequirements2KHR: %s: VR off\n", rs(r));
            return 0;
        }
        nvk_set_create_hooks(&s_hooks_vk);
    }
    X.ok = 1;
    /* The settings (xbox_settings.c) read this: in VR the VR page's picture quality (scale, anti-aliasing...) replaces the Graphics page's. */
#if defined(_WIN32)
    _putenv_s("RECOMP_VR_ACTIVE", "1");
#else
    setenv("RECOMP_VR_ACTIVE", "1", 1);
#endif
    fprintf(stderr, VRLOG "headset \"%s\": through %s%s\n", X.sysname, X.d3d ? "Direct3D 11 (shared with Vulkan)" : "Vulkan",
            X.d3d ? "" : ", the Vulkan device made by the OpenXR runtime");
    return 1;
}

int xbox_VrActive(void)
{
    return X.ok && !X.failed && !X.ended;
}

/* ---- the session ---------------------------------------------------------------------------------- */

static void fail(const char *why)
{
    if (!X.failed)
        fprintf(stderr, VRLOG "%s: the headset output stops (the window goes on)\n", why);
    X.failed = 1;
}

static int make_session(const struct nvk_core *C)
{
    XrSessionCreateInfo si;
    XrReferenceSpaceCreateInfo rsi;
    XrResult r;
#if defined(OXR_HAVE_D3D11)
    XrGraphicsBindingD3D11KHR gd;
#endif
    XrGraphicsBindingVulkan2KHR gv;

    memset(&si, 0, sizeof si);
    si.type = XR_TYPE_SESSION_CREATE_INFO;
    si.systemId = X.sys;
    if (X.d3d) {
#if defined(OXR_HAVE_D3D11)
        IDXGIFactory1 *fac = NULL;
        IDXGIAdapter1 *ad = NULL, *pick = NULL;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        UINT k;

        STEP("the Direct3D 11 device");
        if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac)))
            return 0;
        for (k = 0; IDXGIFactory1_EnumAdapters1(fac, k, &ad) == S_OK; k++) {
            DXGI_ADAPTER_DESC1 d;

            IDXGIAdapter1_GetDesc1(ad, &d);
            if (d.AdapterLuid.LowPart == X.luid.LowPart && d.AdapterLuid.HighPart == X.luid.HighPart)
                pick = ad;
        }
        if (!pick || FAILED(D3D11CreateDevice((IDXGIAdapter *)pick, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &X.dev, NULL,
                                              &X.ctx))) {
            fprintf(stderr, VRLOG "no Direct3D 11 device on the headset's GPU\n");
            return 0;
        }
        memset(&gd, 0, sizeof gd);
        gd.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
        gd.device = X.dev;
        si.next = &gd;
#endif
    } else {
        memset(&gv, 0, sizeof gv);
        gv.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
        gv.instance = C->inst;
        gv.physicalDevice = C->phys;
        gv.device = C->dev;
        gv.queueFamilyIndex = C->qfam;
        gv.queueIndex = 0;
        si.next = &gv;
    }
    STEP("xrCreateSession");
    nvk_queue_lock();
    r = p_xrCreateSession(X.inst, &si, &X.sess);
    nvk_queue_unlock();
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateSession: %s\n", rs(r));
        return 0;
    }
    memset(&rsi, 0, sizeof rsi);
    rsi.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    STEP("xrCreateReferenceSpace");
    r = p_xrCreateReferenceSpace(X.sess, &rsi, &X.space);
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateReferenceSpace: %s\n", rs(r));
        return 0;
    }
    X.session_made = 1;
    fprintf(stderr, VRLOG "session made (%s)\n", X.d3d ? "Direct3D 11" : "on the renderer's Vulkan device and queue");
    return 1;
}

static void poll_events(void)
{
    XrEventDataBuffer ev;

    for (;;) {
        XrResult r;

        memset(&ev, 0, sizeof ev);
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        r = p_xrPollEvent(X.inst, &ev);
        if (r != XR_SUCCESS)
            break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;

            if (e->state == XR_SESSION_STATE_READY && !X.running) {
                XrSessionBeginInfo bi;

                memset(&bi, 0, sizeof bi);
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                STEP("xrBeginSession");
                nvk_queue_lock();
                r = p_xrBeginSession(X.sess, &bi);
                nvk_queue_unlock();
                if (XR_FAILED(r))
                    fprintf(stderr, VRLOG "xrBeginSession: %s\n", rs(r));
                else {
                    X.running = 1;
                    fprintf(stderr, VRLOG "the session is running\n");
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING && X.running) {
                p_xrEndSession(X.sess);
                X.running = 0;
                fprintf(stderr, VRLOG "the session stopped\n");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                X.running = 0;
                X.ended = 1;
                fprintf(stderr, VRLOG "the session is ending (the headset or SteamVR was closed)\n");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            X.running = 0;
            X.ended = 1;
            fprintf(stderr, VRLOG "the runtime is going away\n");
        }
    }
}

#if defined(OXR_HAVE_D3D11)
/* The shared texture: a D3D11 texture with an NT handle, and a Vulkan image on its memory. */
static int make_shared(const struct nvk_core *C, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    IDXGIResource1 *res = NULL;
    HANDLE hnd = NULL;
    VkExternalMemoryImageCreateInfo emi;
    VkImageCreateInfo ici;
    VkMemoryRequirements mr;
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryDedicatedAllocateInfo ded;
    VkImportMemoryWin32HandleInfoKHR imp;
    VkMemoryAllocateInfo mai;
    uint32_t i, ti = 0xFFFFFFFFu;
    VkResult vr;

    STEP("the shared texture");
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(ID3D11Device_CreateTexture2D(X.dev, &td, NULL, &X.shared))) {
        fprintf(stderr, VRLOG "cannot make the shared Direct3D 11 texture\n");
        return 0;
    }
    if (FAILED(ID3D11Texture2D_QueryInterface(X.shared, &IID_IDXGIResource1, (void **)&res))
            || FAILED(IDXGIResource1_CreateSharedHandle(res, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                         NULL, &hnd))) {
        fprintf(stderr, VRLOG "cannot share the Direct3D 11 texture (no NT handle)\n");
        return 0;
    }
    IDXGIResource1_Release(res);
    STEP("the Vulkan image on the shared memory");
    memset(&emi, 0, sizeof emi);
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vr = vkCreateImage(C->dev, &ici, NULL, &X.vkimg);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, VRLOG "vkCreateImage for the shared texture: %d\n", (int)vr);
        return 0;
    }
    vkGetImageMemoryRequirements(C->dev, X.vkimg, &mr);
    vkGetPhysicalDeviceMemoryProperties(C->phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            ti = i;
            break;
        }
    if (ti == 0xFFFFFFFFu) {
        fprintf(stderr, VRLOG "no device-local memory type for the shared texture\n");
        return 0;
    }
    memset(&ded, 0, sizeof ded);
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = X.vkimg;
    memset(&imp, 0, sizeof imp);
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    imp.handle = hnd;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ti;
    vr = vkAllocateMemory(C->dev, &mai, NULL, &X.vkmem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, VRLOG "vkAllocateMemory importing the Direct3D 11 texture: %d (the Vulkan device lacks"
                " VK_KHR_external_memory_win32, or the texture's memory is not importable)\n", (int)vr);
        return 0;
    }
    vr = vkBindImageMemory(C->dev, X.vkimg, X.vkmem, 0);
    CloseHandle(hnd);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, VRLOG "vkBindImageMemory: %d\n", (int)vr);
        return 0;
    }
    X.vkimg_ready = 1;
    return 1;
}
#endif

static int make_swapchain(const struct nvk_core *C, uint32_t w, uint32_t h)
{
    uint32_t n = 0, i;
    int64_t *fmts, pick = 0;
    XrSwapchainCreateInfo ci;
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    XrResult r;
    int64_t want[2] = { 0, 0 };

    if (X.d3d) {
#if defined(OXR_HAVE_D3D11)
        want[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;      /* raw bytes: B8G8R8A8 on both sides */
        want[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (getenv("RECOMP_VR_FORMAT") && !strcmp(getenv("RECOMP_VR_FORMAT"), "unorm")) {   /* the other way round: brighter? */
            want[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
            want[1] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
#endif
    } else {
        want[0] = VK_FORMAT_B8G8R8A8_SRGB;
        want[1] = VK_FORMAT_B8G8R8A8_UNORM;
    }
    STEP("xrEnumerateSwapchainFormats");
    p_xrEnumerateSwapchainFormats(X.sess, 0, &n, NULL);
    fmts = (int64_t *)calloc(n ? n : 1, sizeof *fmts);
    if (!fmts)
        return 0;
    p_xrEnumerateSwapchainFormats(X.sess, n, &n, fmts);
    for (i = 0; i < n; i++)
        if (fmts[i] == want[0])
            pick = fmts[i];
    for (i = 0; i < n && !pick; i++)
        if (fmts[i] == want[1])
            pick = fmts[i];
    if (!pick) {
        fprintf(stderr, VRLOG "the runtime offers no B8G8R8A8 swapchain format (%u formats): the frame cannot be copied raw\n", n);
        free(fmts);
        return 0;
    }
    free(fmts);
    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
                  | (X.d3d ? 0 : XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
    ci.format = pick;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    STEP("xrCreateSwapchain");
    nvk_queue_lock();
    r = p_xrCreateSwapchain(X.sess, &ci, &X.sc);
    nvk_queue_unlock();
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrCreateSwapchain %ux%u: %s\n", w, h, rs(r));
        return 0;
    }
    STEP("xrEnumerateSwapchainImages (count)");
    p_xrEnumerateSwapchainImages(X.sc, 0, &X.nimg, NULL);
    X.cbs = (VkCommandBuffer *)calloc(X.nimg, sizeof *X.cbs);
    X.fences = (VkFence *)calloc(X.nimg, sizeof *X.fences);
    if (X.d3d) {
#if defined(OXR_HAVE_D3D11)
        XrSwapchainImageD3D11KHR *im = (XrSwapchainImageD3D11KHR *)calloc(X.nimg, sizeof *im);

        for (i = 0; im && i < X.nimg; i++)
            im[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        X.imgs = im;
#endif
    } else {
        XrSwapchainImageVulkan2KHR *im = (XrSwapchainImageVulkan2KHR *)calloc(X.nimg, sizeof *im);

        for (i = 0; im && i < X.nimg; i++)
            im[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        X.imgs = im;
    }
    if (!X.imgs || !X.cbs || !X.fences)
        return 0;
    STEP("xrEnumerateSwapchainImages (images)");
    r = p_xrEnumerateSwapchainImages(X.sc, X.nimg, &X.nimg, (XrSwapchainImageBaseHeader *)X.imgs);
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrEnumerateSwapchainImages: %s\n", rs(r));
        return 0;
    }
    STEP("the command pool, buffers and fences");
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = C->qfam;
    if (vkCreateCommandPool(C->dev, &pi, NULL, &X.pool) != VK_SUCCESS)
        return 0;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = X.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = X.nimg;
    if (vkAllocateCommandBuffers(C->dev, &ai, X.cbs) != VK_SUCCESS)
        return 0;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (i = 0; i < X.nimg; i++)
        if (vkCreateFence(C->dev, &fi, NULL, &X.fences[i]) != VK_SUCCESS)
            return 0;
#if defined(OXR_HAVE_D3D11)
    if (X.d3d && !make_shared(C, w, h))
        return 0;
#endif
    X.sw = w;
    X.sh = h;
    X.sfmt = pick;
    fprintf(stderr, VRLOG "swapchain %ux%u, %u images, format %lld (%s)\n", w, h, X.nimg, (long long)pick,
            X.d3d ? "DXGI" : "Vulkan");
    return 1;
}

/* The raw copy of the frame: into the runtime's Vulkan image `idx` (the Vulkan back end), or into the shared texture (the
 * Direct3D 11 back end, copied on into the runtime's image afterwards by the caller). Submitted; with `wait`, waited for. */
static VkImageLayout s_slot_layout = VK_IMAGE_LAYOUT_GENERAL;     /* the frame's layout (an anti-aliased one is a transfer source) */

static int copy_frame(const struct nvk_core *C, VkImage slot, uint32_t idx, int wait)
{
    VkCommandBuffer cb = X.cbs[idx];
    VkCommandBufferBeginInfo bi;
    VkImageCopy ic;
    VkSubmitInfo si;
    VkResult r;
    VkImage dst;
    VkImageLayout end_layout;

#if defined(OXR_HAVE_D3D11)
    if (X.d3d) {
        dst = X.vkimg;
        end_layout = VK_IMAGE_LAYOUT_GENERAL;
    } else
#endif
    {
        dst = ((XrSwapchainImageVulkan2KHR *)X.imgs)[idx].image;
        end_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    vkWaitForFences(C->dev, 1, &X.fences[idx], VK_TRUE, UINT64_MAX);
    vkResetFences(C->dev, 1, &X.fences[idx]);
    vkResetCommandBuffer(cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    nvk_image_barrier(cb, slot, VK_IMAGE_ASPECT_COLOR_BIT, s_slot_layout, s_slot_layout);
    nvk_image_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&ic, 0, sizeof ic);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = X.sw;
    ic.extent.height = X.sh;
    ic.extent.depth = 1;
    vkCmdCopyImage(cb, slot, s_slot_layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    nvk_image_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, end_layout);
    vkEndCommandBuffer(cb);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    nvk_queue_lock();
    r = vkQueueSubmit(C->queue, 1, &si, X.fences[idx]);
    nvk_queue_unlock();
    if (r != VK_SUCCESS)
        return 0;
    if (wait)       /* the other API reads what this wrote: the CPU waits for it */
        vkWaitForFences(C->dev, 1, &X.fences[idx], VK_TRUE, UINT64_MAX);
    return 1;
}

#if defined(OXR_HAVE_D3D11)
/* One swapchain per eye (the Direct3D 11 back end): the runtime is given each eye's own image, not a rectangle of a shared
 * one (the rectangles were not honoured: each eye showed both halves). Each frame's half is copied across by the GPU. */
static XrSwapchain s_esc[2];
static XrSwapchainImageD3D11KHR *s_eimg[2];
static uint32_t s_en[2];
static int s_eye_state;                         /* 0 not tried, 1 made, -1 failed */

static int make_eye_swapchains(uint32_t hw, uint32_t h)
{
    int e;

    for (e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci;
        uint32_t i;
        XrResult r;

        memset(&ci, 0, sizeof ci);
        ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = X.sfmt;
        ci.sampleCount = 1;
        ci.width = hw;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        nvk_queue_lock();
        r = p_xrCreateSwapchain(X.sess, &ci, &s_esc[e]);
        nvk_queue_unlock();
        if (XR_FAILED(r)) {
            fprintf(stderr, VRLOG "eye swapchain %d (%ux%u): %s\n", e, hw, h, rs(r));
            return 0;
        }
        p_xrEnumerateSwapchainImages(s_esc[e], 0, &s_en[e], NULL);
        s_eimg[e] = (XrSwapchainImageD3D11KHR *)calloc(s_en[e], sizeof *s_eimg[e]);
        if (!s_eimg[e])
            return 0;
        for (i = 0; i < s_en[e]; i++)
            s_eimg[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        if (XR_FAILED(p_xrEnumerateSwapchainImages(s_esc[e], s_en[e], &s_en[e], (XrSwapchainImageBaseHeader *)s_eimg[e])))
            return 0;
    }
    fprintf(stderr, VRLOG "one swapchain per eye, %ux%u\n", hw, h);
    return 1;
}

/* The shared texture's half `e` into eye e's next image. 1 when both are done. */
static int copy_eyes(uint32_t w, uint32_t h)
{
    int e, ok = 1;

    for (e = 0; e < 2; e++) {
        XrSwapchainImageAcquireInfo ai;
        XrSwapchainImageWaitInfo wsi;
        XrSwapchainImageReleaseInfo ri;
        uint32_t idx = 0;
        XrResult r;

        memset(&ai, 0, sizeof ai);
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        nvk_queue_lock();
        r = p_xrAcquireSwapchainImage(s_esc[e], &ai, &idx);
        nvk_queue_unlock();
        if (XR_FAILED(r)) { ok = 0; continue; }
        memset(&wsi, 0, sizeof wsi);
        wsi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wsi.timeout = XR_INFINITE_DURATION;
        r = p_xrWaitSwapchainImage(s_esc[e], &wsi);
        if (XR_SUCCEEDED(r)) {
            D3D11_BOX box;

            box.left = (UINT)e * (w / 2u);
            box.right = box.left + w / 2u;
            box.top = 0;
            box.bottom = h;
            box.front = 0;
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(X.ctx, (ID3D11Resource *)s_eimg[e][idx].texture, 0, 0, 0, 0,
                                                      (ID3D11Resource *)X.shared, 0, &box);
        } else
            ok = 0;
        memset(&ri, 0, sizeof ri);
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        nvk_queue_lock();
        p_xrReleaseSwapchainImage(s_esc[e], &ri);
        nvk_queue_unlock();
    }
    ID3D11DeviceContext_Flush(X.ctx);
    return ok;
}
#endif

void xbox_VrPresent(const struct nvk_core *C, VkImage slot, VkImageLayout slot_layout, uint32_t w, uint32_t h, VkFormat fmt,
                    uint32_t stereo, uint32_t flip)
{
    XrFrameWaitInfo wi;
    XrFrameState fs;
    XrFrameBeginInfo bgi;
    XrFrameEndInfo ei;
    XrCompositionLayerProjection lp;
    XrCompositionLayerProjectionView pv[2];
    const XrCompositionLayerBaseHeader *layers[1];
    XrResult r;
    uint32_t idx = 0;
    int have_layer = 0, have_pose = 0;
    float k, vwx = 0, vwy = 0, ipd = 0;
    xbox_vr_pose pose;
    int menu;
    XrCompositionLayerQuad lq;

    memset(&pose, 0, sizeof pose);
    (void)fmt;
    (void)flip;
    s_slot_layout = slot_layout;
    if (!xbox_VrActive() || !C || !w || !h)
        return;
    menu = !(stereo & NV2A_STEREO_ON);       /* menus and loading: the whole picture on a screen in front of the player */
    if (!X.session_made && !make_session(C)) {
        fail("no session");
        return;
    }
    poll_events();
    if (!X.running)
        return;                              /* READY has not come yet, or the session paused: the window goes on */
    if (!X.sc && !make_swapchain(C, w, h)) {
        fail("no swapchain");
        return;
    }
    if (w != X.sw || h != X.sh) {
        static int said;

        if (said++ < 3)
            fprintf(stderr, VRLOG "a %ux%u frame, the swapchain is %ux%u: not shown in the headset%s" "\n", w, h, X.sw, X.sh,
                    menu ? " (a menu frame)" : "");
        return;                              /* the picture's size changed: one swapchain size per run */
    }
    memset(&wi, 0, sizeof wi);
    wi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof fs);
    fs.type = XR_TYPE_FRAME_STATE;
    STEP("xrWaitFrame");
    r = p_xrWaitFrame(X.sess, &wi, &fs);
    if (XR_FAILED(r)) {
        fail("xrWaitFrame");
        return;
    }
    memset(&bgi, 0, sizeof bgi);
    bgi.type = XR_TYPE_FRAME_BEGIN_INFO;
    STEP("xrBeginFrame");
    nvk_queue_lock();
    r = p_xrBeginFrame(X.sess, &bgi);
    nvk_queue_unlock();
    if (XR_FAILED(r)) {
        fprintf(stderr, VRLOG "xrBeginFrame: %s\n", rs(r));
        return;
    }
    /* The head now, for the next picture the game draws. */
    {
        XrViewLocateInfo vli;
        XrViewState vs;
        XrView views[2];
        uint32_t nv = 0;

        memset(&vli, 0, sizeof vli);
        vli.type = XR_TYPE_VIEW_LOCATE_INFO;
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = X.space;
        memset(&vs, 0, sizeof vs);
        vs.type = XR_TYPE_VIEW_STATE;
        memset(views, 0, sizeof views);
        views[0].type = views[1].type = XR_TYPE_VIEW;
        if (XR_SUCCEEDED(p_xrLocateViews(X.sess, &vli, &vs, 2, &nv, views)) && nv == 2
                && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            xbox_vr_pose np;
            float dx = views[1].pose.position.x - views[0].pose.position.x;
            float dy = views[1].pose.position.y - views[0].pose.position.y;
            float dz = views[1].pose.position.z - views[0].pose.position.z;
            float t, m;
            int e;

            memset(&np, 0, sizeof np);
            np.q[0] = views[0].pose.orientation.x;
            np.q[1] = views[0].pose.orientation.y;
            np.q[2] = views[0].pose.orientation.z;
            np.q[3] = views[0].pose.orientation.w;
            np.p[0] = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
            np.p[1] = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
            np.p[2] = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
            np.ipd = sqrtf(dx * dx + dy * dy + dz * dz);
            if (np.ipd < 0.03f)
                np.ipd = 0.063f;                /* the runtime gave both eyes one place: use a typical separation */
            for (e = 0; e < 2; e++) {           /* the wider side of each, over both eyes */
                t = tanf(-views[e].fov.angleLeft);
                if (t > np.vwx) np.vwx = t;
                t = tanf(views[e].fov.angleRight);
                if (t > np.vwx) np.vwx = t;
                t = tanf(views[e].fov.angleUp);
                if (t > np.vwy) np.vwy = t;
                t = tanf(-views[e].fov.angleDown);
                if (t > np.vwy) np.vwy = t;
            }
            m = np.q[0] * np.q[0] + np.q[1] * np.q[1] + np.q[2] * np.q[2] + np.q[3] * np.q[3];
            if (m > 0.5f && np.vwx > 0.1f && np.vwy > 0.1f) {
                np.id = s_next_id;
                s_next_id = s_next_id % 255u + 1u;               /* 1..255 */
                s_ring[np.id].pose = np;
                __atomic_store_n(&s_pose_seq, s_pose_seq + 1u, __ATOMIC_RELEASE);
                s_latest = np;
                __atomic_store_n(&s_pose_seq, s_pose_seq + 1u, __ATOMIC_RELEASE);
            }
        }
    }
    k = nv2a_stereo_k(stereo);
    {
        unsigned pid = nv2a_stereo_pose(stereo);

        if (pid && s_ring[pid].pose.id == pid) {
            pose = s_ring[pid].pose;            /* what the picture was drawn from */
            have_pose = 1;
        } else if (!menu && s_latest.id) {
            pose = s_latest;                    /* the id is gone from the ring: the latest will do */
            have_pose = 1;
        }
    }
    vwx = pose.vwx;
    vwy = pose.vwy;
    ipd = pose.ipd;
    (void)k;
    if (!menu)
        s_menu_set = 0;
    if (fs.shouldRender && (have_pose || menu)) {
        XrSwapchainImageAcquireInfo ai;
        XrSwapchainImageWaitInfo wsi;
        int copied = 0;
        uint32_t cbidx = X.frames % X.nimg;      /* the Direct3D 11 path's command buffer; the Vulkan path's is the image's */

        if (X.d3d) {
            STEP("the copy into the shared texture");
            copied = copy_frame(C, slot, cbidx, 1);
        }
        memset(&ai, 0, sizeof ai);
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        STEP("xrAcquireSwapchainImage");
        nvk_queue_lock();
        r = p_xrAcquireSwapchainImage(X.sc, &ai, &idx);
        nvk_queue_unlock();
        if (XR_SUCCEEDED(r)) {
            memset(&wsi, 0, sizeof wsi);
            wsi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            wsi.timeout = XR_INFINITE_DURATION;
            STEP("xrWaitSwapchainImage");
            r = p_xrWaitSwapchainImage(X.sc, &wsi);
        }
        if (XR_SUCCEEDED(r)) {
            if (!X.d3d) {
                STEP("the copy into the runtime's Vulkan image");
                copied = copy_frame(C, slot, idx, 0);
            }
#if defined(OXR_HAVE_D3D11)
            else if (copied) {
                STEP("CopyResource into the runtime's Direct3D 11 image");
                ID3D11DeviceContext_CopyResource(X.ctx, (ID3D11Resource *)((XrSwapchainImageD3D11KHR *)X.imgs)[idx].texture,
                                                 (ID3D11Resource *)X.shared);
                ID3D11DeviceContext_Flush(X.ctx);
            }
#endif
        }
        if (XR_SUCCEEDED(r) && copied) {
            XrSwapchainImageReleaseInfo ri;
            int e;

            memset(&ri, 0, sizeof ri);
            ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            STEP("xrReleaseSwapchainImage");
            nvk_queue_lock();
            p_xrReleaseSwapchainImage(X.sc, &ri);
            nvk_queue_unlock();
#if defined(OXR_HAVE_D3D11)
            if (!menu && X.d3d && s_eye_state >= 0) {
                if (!s_eye_state)
                    s_eye_state = make_eye_swapchains(w / 2u, h) ? 1 : -1;
                if (s_eye_state > 0 && !copy_eyes(w, h))
                    s_eye_state = -1;
            }
#endif
            if (menu) {
                if (!s_menu_set) {              /* put the screen 2 m ahead of where the player looks now, and leave it there */
                    xbox_vr_pose hp;
                    float fx, fz, n, a;

                    memset(&hp, 0, sizeof hp);
                    hp.q[3] = 1.0f;
                    hp.p[1] = 1.2f;
                    if (s_latest.id)
                        hp = s_latest;
                    fx = -2.0f * (hp.q[0] * hp.q[2] + hp.q[3] * hp.q[1]);
                    fz = -(1.0f - 2.0f * (hp.q[0] * hp.q[0] + hp.q[1] * hp.q[1]));
                    n = sqrtf(fx * fx + fz * fz);
                    if (n < 0.1f) {
                        fx = 0.0f;
                        fz = -1.0f;
                    } else {
                        fx /= n;
                        fz /= n;
                    }
                    a = atan2f(-fx, -fz);
                    s_menu_pose[0] = 0.0f;
                    s_menu_pose[1] = sinf(0.5f * a);
                    s_menu_pose[2] = 0.0f;
                    s_menu_pose[3] = cosf(0.5f * a);
                    s_menu_pose[4] = hp.p[0] + 2.0f * fx;
                    s_menu_pose[5] = hp.p[1];
                    s_menu_pose[6] = hp.p[2] + 2.0f * fz;
                    s_menu_set = 1;
                }
                memset(&lq, 0, sizeof lq);
                lq.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                lq.space = X.space;
                lq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                lq.subImage.swapchain = X.sc;
                lq.subImage.imageRect.extent.width = (int32_t)w;
                lq.subImage.imageRect.extent.height = (int32_t)h;
                lq.pose.orientation.y = s_menu_pose[1];
                lq.pose.orientation.w = s_menu_pose[3];
                lq.pose.position.x = s_menu_pose[4];
                lq.pose.position.y = s_menu_pose[5];
                lq.pose.position.z = s_menu_pose[6];
                /* between 4:3 (a little tall) and 16:9 (squashed): 3:2 */
                {
                    /* RECOMP_VR_MENU_ASPECT: width / height, default 1.42; RECOMP_VR_SCREEN_X / _Y: the screen's width and
                     * height each stretched or squeezed by that percent (0 = none). Read each frame: the VR page changes them live. */
                    float aspect = (float)recomp_env_double(RENV_VR_MENU_ASPECT, 1.42);
                    float sx = 1.0f + (float)recomp_env_double(RENV_VR_SCREEN_X, 0.0) / 100.0f;
                    float sy = 1.0f + (float)recomp_env_double(RENV_VR_SCREEN_Y, 0.0) / 100.0f;

                    if (!(aspect > 1.0f && aspect < 2.5f))
                        aspect = 1.42f;
                    if (!(sx > 0.3f && sx < 3.0f)) sx = 1.0f;
                    if (!(sy > 0.3f && sy < 3.0f)) sy = 1.0f;
                    lq.size.width = 2.9f * sx;
                    lq.size.height = 2.9f / aspect * sy;
                }
                layers[0] = (const XrCompositionLayerBaseHeader *)&lq;
                have_layer = 1;
            } else {
            memset(&lp, 0, sizeof lp);
            lp.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            lp.space = X.space;
            lp.viewCount = 2;
            lp.views = pv;
            for (e = 0; e < 2; e++) {
                /* The eye: the head's pose, half the eye separation to the left or right along the head's own right axis. */
                float qx = pose.q[0], qy = pose.q[1], qz = pose.q[2], qw = pose.q[3];
                float rx = 1.0f - 2.0f * (qy * qy + qz * qz), ry = 2.0f * (qx * qy + qw * qz), rz = 2.0f * (qx * qz - qw * qy);
                float side = (e ? 0.5f : -0.5f) * ipd;

                memset(&pv[e], 0, sizeof pv[e]);
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[e].pose.orientation.x = qx;
                pv[e].pose.orientation.y = qy;
                pv[e].pose.orientation.z = qz;
                pv[e].pose.orientation.w = qw;
                pv[e].pose.position.x = pose.p[0] + side * rx;
                pv[e].pose.position.y = pose.p[1] + side * ry;
                pv[e].pose.position.z = pose.p[2] + side * rz;
                /* The VR page's picture size adjust (RECOMP_VR_ASPECT_X / _Y, percent, 0 = as drawn): the layer claims a wider or
                 * narrower frustum than the picture was drawn with, so the picture is stretched or squeezed on that axis. Read
                 * each frame, so the settings page changes it live. */
                float ax = 1.0f + (float)recomp_env_double(RENV_VR_ASPECT_X, 0.0) / 100.0f;
                float ay = 1.0f + (float)recomp_env_double(RENV_VR_ASPECT_Y, 0.0) / 100.0f;

                if (!(ax > 0.3f && ax < 3.0f)) ax = 1.0f;
                if (!(ay > 0.3f && ay < 3.0f)) ay = 1.0f;
                pv[e].fov.angleLeft = -atanf(vwx * ax);
                pv[e].fov.angleRight = atanf(vwx * ax);
                pv[e].fov.angleUp = atanf(vwy * ay);
                pv[e].fov.angleDown = -atanf(vwy * ay);
                pv[e].subImage.swapchain = X.sc;
                pv[e].subImage.imageRect.offset.x = (int32_t)(((X.swap ? 1 - e : e)) * (w / 2u));   /* RECOMP_VR_SWAP=1 */
#if defined(OXR_HAVE_D3D11)
                if (s_eye_state > 0) {
                    pv[e].subImage.swapchain = s_esc[X.swap ? 1 - e : e];
                    pv[e].subImage.imageRect.offset.x = 0;
                }
#endif
                pv[e].subImage.imageRect.extent.width = (int32_t)(w / 2u);
                pv[e].subImage.imageRect.extent.height = (int32_t)h;
            }
            layers[0] = (const XrCompositionLayerBaseHeader *)&lp;
            have_layer = 1;
            }
        } else if (XR_SUCCEEDED(r)) {
            XrSwapchainImageReleaseInfo ri;

            memset(&ri, 0, sizeof ri);
            ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            p_xrReleaseSwapchainImage(X.sc, &ri);
        }
    }
    memset(&ei, 0, sizeof ei);
    ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = have_layer ? 1u : 0u;
    ei.layers = have_layer ? layers : NULL;
    STEP("xrEndFrame");
    nvk_queue_lock();
    r = p_xrEndFrame(X.sess, &ei);
    nvk_queue_unlock();
    if (XR_FAILED(r))
        fprintf(stderr, VRLOG "xrEndFrame: %s\n", rs(r));
    if (X.frames++ == 0)
        fprintf(stderr, VRLOG "first frame to the headset (pose %u): eye separation %.3f m, view window %.3f x %.3f "
                "(field of view %.0f x %.0f degrees)%s\n", pose.id, ipd, vwx, vwy, 2.0 * atan((double)vwx) * 57.29578,
                2.0 * atan((double)vwy) * 57.29578, X.swap ? ", eyes swapped" : "");
}

void xbox_VrShutdown(void)
{
    if (!X.inited || !X.inst)
        return;
    if (X.running && X.sess)
        p_xrEndSession(X.sess);
    if (X.sc)
        p_xrDestroySwapchain(X.sc);
    if (X.space)
        p_xrDestroySpace(X.space);
    if (X.sess)
        p_xrDestroySession(X.sess);
    p_xrDestroyInstance(X.inst);
    memset(&X, 0, sizeof X);
}

#else /* no OpenXR headers or no Vulkan: stubs */

#include <stdio.h>

int xbox_VrInit(void)
{
    fprintf(stderr, "[VR] this build has no OpenXR support (XBOXRECOMP_OPENXR_INCLUDE_DIR): VR off\n");
    return 0;
}
int xbox_VrActive(void) { return 0; }
void xbox_VrPresent(const struct nvk_core *C, VkImage slot, VkImageLayout slot_layout, uint32_t w, uint32_t h, VkFormat fmt,
                    uint32_t stereo, uint32_t flip)
{
    (void)C; (void)slot; (void)slot_layout; (void)w; (void)h; (void)fmt; (void)stereo; (void)flip;
}
void xbox_VrRecentre(void) {}
void xbox_VrFrame(unsigned id, float vwx, float vwy) { (void)id; (void)vwx; (void)vwy; }
void xbox_VrShutdown(void) {}

#endif
