/**
 * OpenXR on the Vulkan backend's device. See nv2a_vk_xr.h.
 *
 * Follows the Steam Frame ports' conventions: the Frame controller profile
 * bound first, Index and Touch as fallbacks; sRGB swapchains with the
 * title's already gamma-encoded picture copied in bit for bit (a copy, not a
 * blit, so nothing converts it again); a projection layer in every frame;
 * "[openxr]" and "[vr-perf]" lines in the log.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "nv2a_vk_core.h"
#include "nv2a_vk_xr.h"
#include "input/input_backend.h"
#include "kernel/nv2a_backend_common.h"

#define FRAME_PROFILE "/interaction_profiles/valve/frame_controller_valve"
#define SLOTS 3

typedef struct {
    XrSwapchain handle;
    uint32_t w, h, count;
    XrSwapchainImageVulkan2KHR *images;
} Swapchain;

static struct {
    int wanted;                 /* -1 unknown */
    int prepared, started;
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSessionState state;
    volatile int running, focused, quit;
    XrSpace local, view;
    int frame_controller, refresh_ext;
    const struct nvk_core *core;
    /* the screen, its swapchain and the eyes' (black or test colours) */
    Swapchain screen, eye[2];
    int screen_valid;           /* its last released image holds a picture */
    XrViewConfigurationView config_views[2];
    XrView views[2];
    int eyes_drawn;
    float distance, width, aspect;
    XrPosef screen_pose;
    int screen_placed;
    double view_held_since;
    uint32_t crop0, crop1;      /* of 480 */
    /* the picture handoff: slots the backend copies frames into */
    pthread_mutex_t slot_lock;
    struct nvk_image slot[SLOTS];
    int back, ready, front;     /* -1: none */
    uint64_t published;
    /* stereo: each slot's eye images and the views they were drawn for */
    struct nvk_image eye_img[SLOTS][2], hud_img[SLOTS];
    int stereo[SLOTS];
    Swapchain hud;              /* the HUD panel's swapchain */
    int shown_hud;
    float hud_distance, hud_width;
    uint32_t hud_crop0, hud_crop1;
    XrView eye_view[SLOTS][2];
    Swapchain st_eye[2];        /* the stereo eyes' swapchains (the slots' size) */
    int shown_stereo, shown_cinema;
    XrView shown_view[2];
    /* the newest located views (LOCAL) and the anchor, for the backend */
    pthread_mutex_t view_lock;
    XrView latest[2];
    int latest_valid, fake_views;
    XrPosef anchor;
    int anchor_pending;
    float world_scale;
    uint64_t perf_stereo;
    /* the headset thread's command buffer */
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkFence resize_fence;       /* the backend's thread: a slot resize */
    XrTime predicted;
    XrDuration period;
    /* [vr-perf] */
    uint64_t perf_t0, perf_frames, perf_new, perf_missed, perf_wait_ns, perf_published;
    /* RECOMP_XR_DUMP=<prefix>: a picture as <prefix>NNN.ppm every 5 s */
    const char *dump;
    uint64_t dump_last;
    int dump_n;
    struct nvk_buffer dump_buf;
    /* actions */
    XrActionSet set;
    XrAction stick_l, stick_r, trigger_l, trigger_r;
    XrAction a, b, x, y, bumper_l, bumper_r, menu, view_button, thumb_l, thumb_r;
    XrAction dpad_up, dpad_down, dpad_left, dpad_right, haptic;
    XrPath hand[2];
} s_xr = { .wanted = -1, .slot_lock = PTHREAD_MUTEX_INITIALIZER, .view_lock = PTHREAD_MUTEX_INITIALIZER,
            .back = -1, .ready = -1, .front = -1, .anchor = { { 0, 0, 0, 1 }, { 0, 0, 0 } } };

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int xr_ok(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r))
        return 1;
    {
        char name[XR_MAX_RESULT_STRING_SIZE] = "";
        if (s_xr.instance)
            xrResultToString(s_xr.instance, r, name);
        fprintf(stderr, "[openxr] %s failed: %s (%d)\n", what, name, (int)r);
    }
    return 0;
}

static float env_float(const char *name, float fallback)
{
    const char *e = getenv(name);
    return e && *e ? (float)atof(e) : fallback;
}

static PFN_xrVoidFunction xr_fn(const char *name)
{
    PFN_xrVoidFunction f = NULL;
    xrGetInstanceProcAddr(s_xr.instance, name, &f);
    return f;
}

/* ── Input: the Frame controllers as the title's first pad ────────────── */

static pthread_mutex_t s_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static XBOX_INPUT_STATE s_pad;
static int s_pad_valid;
static WORD s_rumble[2];
static int s_rumble_pending;

static DWORD pad_get_state(DWORD port, XBOX_INPUT_STATE *out)
{
    if (port == 0) {
        int ok;
        pthread_mutex_lock(&s_pad_lock);
        ok = s_pad_valid;
        if (ok)
            *out = s_pad;
        pthread_mutex_unlock(&s_pad_lock);
        if (ok)
            return ERROR_SUCCESS;
    }
    return xbox_input_host_backend.get_state(port, out);
}

static DWORD pad_set_state(DWORD port, const XBOX_VIBRATION *vib)
{
    if (port == 0) {
        pthread_mutex_lock(&s_pad_lock);
        if (s_pad_valid) {
            s_rumble[0] = vib->wLeftMotorSpeed;
            s_rumble[1] = vib->wRightMotorSpeed;
            s_rumble_pending = 1;
        }
        pthread_mutex_unlock(&s_pad_lock);
    }
    return xbox_input_host_backend.set_state ? xbox_input_host_backend.set_state(port, vib) : ERROR_SUCCESS;
}

static void pad_init(void)
{
    if (xbox_input_host_backend.init)
        xbox_input_host_backend.init();
}

static void pad_shutdown(void)
{
    if (xbox_input_host_backend.shutdown)
        xbox_input_host_backend.shutdown();
}

static xbox_input_backend s_pad_backend;

static XrAction make_action(const char *name, XrActionType type, int per_hand)
{
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    XrAction action = XR_NULL_HANDLE;

    info.actionType = type;
    snprintf(info.actionName, sizeof info.actionName, "%s", name);
    snprintf(info.localizedActionName, sizeof info.localizedActionName, "%s", name);
    if (per_hand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = s_xr.hand;
    }
    xr_ok(xrCreateAction(s_xr.set, &info, &action), name);
    return action;
}

typedef struct { XrAction *action; const char *path; } Binding;

static void suggest(const char *profile, const Binding *bindings, int count)
{
    XrActionSuggestedBinding suggested[32];
    XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    int i, n = 0;

    for (i = 0; i < count && n < 32; i++) {
        if (!*bindings[i].action)
            continue;
        suggested[n].action = *bindings[i].action;
        if (!XR_SUCCEEDED(xrStringToPath(s_xr.instance, bindings[i].path, &suggested[n].binding)))
            continue;
        n++;
    }
    if (!XR_SUCCEEDED(xrStringToPath(s_xr.instance, profile, &info.interactionProfile)))
        return;
    info.countSuggestedBindings = (uint32_t)n;
    info.suggestedBindings = suggested;
    if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(s_xr.instance, &info)))
        fprintf(stderr, "[openxr] bindings for %s\n", profile);
    else
        fprintf(stderr, "[openxr] %s not accepted\n", profile);
}

static void create_actions(void)
{
    XrActionSetCreateInfo set = { XR_TYPE_ACTION_SET_CREATE_INFO };
    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };

    xrStringToPath(s_xr.instance, "/user/hand/left", &s_xr.hand[0]);
    xrStringToPath(s_xr.instance, "/user/hand/right", &s_xr.hand[1]);
    snprintf(set.actionSetName, sizeof set.actionSetName, "gamepad");
    snprintf(set.localizedActionSetName, sizeof set.localizedActionSetName, "Gamepad");
    if (!xr_ok(xrCreateActionSet(s_xr.instance, &set, &s_xr.set), "xrCreateActionSet"))
        return;
    s_xr.stick_l = make_action("stick_left", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    s_xr.stick_r = make_action("stick_right", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    s_xr.trigger_l = make_action("trigger_left", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.trigger_r = make_action("trigger_right", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.a = make_action("a", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.b = make_action("b", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.x = make_action("x", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.y = make_action("y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.bumper_l = make_action("bumper_left", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.bumper_r = make_action("bumper_right", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.menu = make_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.view_button = make_action("view", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.thumb_l = make_action("thumb_left", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.thumb_r = make_action("thumb_right", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.dpad_up = make_action("dpad_up", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.dpad_down = make_action("dpad_down", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.dpad_left = make_action("dpad_left", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.dpad_right = make_action("dpad_right", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.haptic = make_action("rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
    if (s_xr.frame_controller) {
        /* The Frame's gamepad layout, one to one: ABXY on the right, D-pad on
         * the left, Menu = Start, View = Back, bumpers = Black and White. */
        const Binding frame[] = {
            { &s_xr.stick_l, "/user/hand/left/input/thumbstick" },
            { &s_xr.stick_r, "/user/hand/right/input/thumbstick" },
            { &s_xr.trigger_l, "/user/hand/left/input/trigger/value" },
            { &s_xr.trigger_r, "/user/hand/right/input/trigger/value" },
            { &s_xr.a, "/user/hand/right/input/a/click" },
            { &s_xr.b, "/user/hand/right/input/b/click" },
            { &s_xr.x, "/user/hand/right/input/x/click" },
            { &s_xr.y, "/user/hand/right/input/y/click" },
            { &s_xr.bumper_l, "/user/hand/left/input/bumper/click" },
            { &s_xr.bumper_r, "/user/hand/right/input/bumper/click" },
            { &s_xr.menu, "/user/hand/right/input/menu/click" },
            { &s_xr.view_button, "/user/hand/left/input/view/click" },
            { &s_xr.thumb_l, "/user/hand/left/input/thumbstick/click" },
            { &s_xr.thumb_r, "/user/hand/right/input/thumbstick/click" },
            { &s_xr.dpad_up, "/user/hand/left/input/dpad_up/click" },
            { &s_xr.dpad_down, "/user/hand/left/input/dpad_down/click" },
            { &s_xr.dpad_left, "/user/hand/left/input/dpad_left/click" },
            { &s_xr.dpad_right, "/user/hand/left/input/dpad_right/click" },
            { &s_xr.haptic, "/user/hand/left/output/haptic" },
            { &s_xr.haptic, "/user/hand/right/output/haptic" },
        };
        suggest(FRAME_PROFILE, frame, (int)(sizeof frame / sizeof frame[0]));
    }
    {
        const Binding index[] = {
            { &s_xr.stick_l, "/user/hand/left/input/thumbstick" },
            { &s_xr.stick_r, "/user/hand/right/input/thumbstick" },
            { &s_xr.trigger_l, "/user/hand/left/input/trigger/value" },
            { &s_xr.trigger_r, "/user/hand/right/input/trigger/value" },
            { &s_xr.a, "/user/hand/right/input/a/click" },
            { &s_xr.b, "/user/hand/right/input/b/click" },
            { &s_xr.x, "/user/hand/left/input/a/click" },
            { &s_xr.y, "/user/hand/left/input/b/click" },
            { &s_xr.thumb_l, "/user/hand/left/input/thumbstick/click" },
            { &s_xr.thumb_r, "/user/hand/right/input/thumbstick/click" },
            { &s_xr.menu, "/user/hand/right/input/system/click" },
            { &s_xr.haptic, "/user/hand/left/output/haptic" },
            { &s_xr.haptic, "/user/hand/right/output/haptic" },
        };
        const Binding touch[] = {
            { &s_xr.stick_l, "/user/hand/left/input/thumbstick" },
            { &s_xr.stick_r, "/user/hand/right/input/thumbstick" },
            { &s_xr.trigger_l, "/user/hand/left/input/trigger/value" },
            { &s_xr.trigger_r, "/user/hand/right/input/trigger/value" },
            { &s_xr.a, "/user/hand/right/input/a/click" },
            { &s_xr.b, "/user/hand/right/input/b/click" },
            { &s_xr.x, "/user/hand/left/input/x/click" },
            { &s_xr.y, "/user/hand/left/input/y/click" },
            { &s_xr.bumper_l, "/user/hand/left/input/squeeze/value" },
            { &s_xr.bumper_r, "/user/hand/right/input/squeeze/value" },
            { &s_xr.menu, "/user/hand/left/input/menu/click" },
            { &s_xr.thumb_l, "/user/hand/left/input/thumbstick/click" },
            { &s_xr.thumb_r, "/user/hand/right/input/thumbstick/click" },
            { &s_xr.haptic, "/user/hand/left/output/haptic" },
            { &s_xr.haptic, "/user/hand/right/output/haptic" },
        };
        suggest("/interaction_profiles/valve/index_controller", index, (int)(sizeof index / sizeof index[0]));
        suggest("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof touch / sizeof touch[0]));
    }
    attach.countActionSets = 1;
    attach.actionSets = &s_xr.set;
    xr_ok(xrAttachSessionActionSets(s_xr.session, &attach), "xrAttachSessionActionSets");
}

static int get_bool(XrAction action)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (!action)
        return 0;
    info.action = action;
    return XR_SUCCEEDED(xrGetActionStateBoolean(s_xr.session, &info, &st)) && st.isActive && st.currentState;
}

static float get_float(XrAction action)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    if (!action)
        return 0.0f;
    info.action = action;
    if (!XR_SUCCEEDED(xrGetActionStateFloat(s_xr.session, &info, &st)) || !st.isActive)
        return 0.0f;
    return st.currentState;
}

static XrVector2f get_vec2(XrAction action)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrVector2f zero = { 0.0f, 0.0f };
    if (!action)
        return zero;
    info.action = action;
    if (!XR_SUCCEEDED(xrGetActionStateVector2f(s_xr.session, &info, &st)) || !st.isActive)
        return zero;
    return st.currentState;
}

static SHORT axis(float v)
{
    v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return (SHORT)(v * 32767.0f);
}

static BYTE analog(float v)
{
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (BYTE)(v * 255.0f + 0.5f);
}

/* Returns 1 while View is held, for the recentre gesture. */
static int sync_input(void)
{
    XrActiveActionSet active = { s_xr.set, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    XBOX_INPUT_STATE pad;
    XrVector2f l, r;
    WORD buttons = 0;
    int view, h;

    if (!s_xr.set || !s_xr.focused
            || (sync.countActiveActionSets = 1, sync.activeActionSets = &active,
                !XR_SUCCEEDED(xrSyncActions(s_xr.session, &sync)))) {
        pthread_mutex_lock(&s_pad_lock);
        s_pad_valid = 0;
        pthread_mutex_unlock(&s_pad_lock);
        return 0;
    }
    memset(&pad, 0, sizeof pad);
    l = get_vec2(s_xr.stick_l);
    r = get_vec2(s_xr.stick_r);
    pad.Gamepad.sThumbLX = axis(l.x);
    pad.Gamepad.sThumbLY = axis(l.y);
    pad.Gamepad.sThumbRX = axis(r.x);
    pad.Gamepad.sThumbRY = axis(r.y);
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = analog(get_float(s_xr.trigger_l));
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = analog(get_float(s_xr.trigger_r));
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_A] = get_bool(s_xr.a) ? 255 : 0;
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_B] = get_bool(s_xr.b) ? 255 : 0;
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_X] = get_bool(s_xr.x) ? 255 : 0;
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = get_bool(s_xr.y) ? 255 : 0;
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = get_bool(s_xr.bumper_l) ? 255 : 0;
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = get_bool(s_xr.bumper_r) ? 255 : 0;
    view = get_bool(s_xr.view_button);
    if (get_bool(s_xr.dpad_up)) buttons |= XBOX_GAMEPAD_DPAD_UP;
    if (get_bool(s_xr.dpad_down)) buttons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (get_bool(s_xr.dpad_left)) buttons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (get_bool(s_xr.dpad_right)) buttons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (get_bool(s_xr.menu)) buttons |= XBOX_GAMEPAD_START;
    if (view) buttons |= XBOX_GAMEPAD_BACK;
    if (get_bool(s_xr.thumb_l)) buttons |= XBOX_GAMEPAD_LEFT_THUMB;
    if (get_bool(s_xr.thumb_r)) buttons |= XBOX_GAMEPAD_RIGHT_THUMB;
    pad.Gamepad.wButtons = buttons;

    pthread_mutex_lock(&s_pad_lock);
    s_pad = pad;
    s_pad_valid = 1;
    if (s_rumble_pending && s_xr.haptic) {
        for (h = 0; h < 2; h++) {
            XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
            XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
            float amp = (float)s_rumble[h] / 65535.0f;
            info.action = s_xr.haptic;
            info.subactionPath = s_xr.hand[h];
            if (amp > 0.01f) {
                vib.amplitude = amp;
                vib.duration = 100000000;   /* 100 ms; the title refreshes it */
                vib.frequency = XR_FREQUENCY_UNSPECIFIED;
                xrApplyHapticFeedback(s_xr.session, &info, (const XrHapticBaseHeader *)&vib);
            } else {
                xrStopHapticFeedback(s_xr.session, &info);
            }
        }
        s_rumble_pending = 0;
    }
    pthread_mutex_unlock(&s_pad_lock);
    return view;
}

/* ── Instance, device, session ────────────────────────────────────────── */

static int has_extension(const XrExtensionProperties *list, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(list[i].extensionName, name))
            return 1;
    return 0;
}

int nvkxr_wanted(void)
{
    if (s_xr.wanted < 0) {
        const char *e = getenv("RECOMP_XR");
        s_xr.wanted = e && *e == '1';
    }
    return s_xr.wanted;
}

int nvkxr_prepare(void)
{
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    XrExtensionProperties *exts;
    const char *enable[4];
    uint32_t n = 0, count = 0;

    if (s_xr.prepared || !nvkxr_wanted())
        return s_xr.prepared;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    exts = (XrExtensionProperties *)calloc(n ? n : 1, sizeof *exts);
    for (count = 0; count < n; count++)
        exts[count].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, exts);
    if (!has_extension(exts, n, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        fprintf(stderr, "[openxr] the runtime has no %s; no headset\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        free(exts);
        s_xr.wanted = 0;
        return 0;
    }
    count = 0;
    enable[count++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    s_xr.frame_controller = has_extension(exts, n, "XR_VALVE_frame_controller_interaction");
    if (s_xr.frame_controller)
        enable[count++] = "XR_VALVE_frame_controller_interaction";
    s_xr.refresh_ext = has_extension(exts, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (s_xr.refresh_ext)
        enable[count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    free(exts);

    snprintf(ici.applicationInfo.applicationName, sizeof ici.applicationInfo.applicationName, "Takedawn VR");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName, sizeof ici.applicationInfo.engineName, "xboxrecomp");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = count;
    ici.enabledExtensionNames = enable;
    if (!xr_ok(xrCreateInstance(&ici, &s_xr.instance), "xrCreateInstance")) {
        s_xr.wanted = 0;
        return 0;
    }
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_ok(xrGetSystem(s_xr.instance, &sgi, &s_xr.system), "xrGetSystem")) {
        xrDestroyInstance(s_xr.instance);
        s_xr.instance = XR_NULL_HANDLE;
        s_xr.wanted = 0;
        return 0;
    }
    {
        PFN_xrGetVulkanGraphicsRequirements2KHR req_fn =
            (PFN_xrGetVulkanGraphicsRequirements2KHR)xr_fn("xrGetVulkanGraphicsRequirements2KHR");
        XrGraphicsRequirementsVulkan2KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
        if (req_fn && xr_ok(req_fn(s_xr.instance, s_xr.system, &req), "xrGetVulkanGraphicsRequirements2KHR"))
            fprintf(stderr, "[openxr] Vulkan %u.%u to %u.%u\n",
                    (unsigned)XR_VERSION_MAJOR(req.minApiVersionSupported),
                    (unsigned)XR_VERSION_MINOR(req.minApiVersionSupported),
                    (unsigned)XR_VERSION_MAJOR(req.maxApiVersionSupported),
                    (unsigned)XR_VERSION_MINOR(req.maxApiVersionSupported));
    }
    s_xr.prepared = 1;
    fprintf(stderr, "[openxr] instance ready (frame controller %s)\n", s_xr.frame_controller ? "yes" : "no");
    return 1;
}

VkResult nvkxr_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out)
{
    PFN_xrCreateVulkanInstanceKHR fn = (PFN_xrCreateVulkanInstanceKHR)xr_fn("xrCreateVulkanInstanceKHR");
    XrVulkanInstanceCreateInfoKHR info = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;

    if (!fn)
        return vr;
    info.systemId = s_xr.system;
    info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    info.vulkanCreateInfo = ci;
    if (!xr_ok(fn(s_xr.instance, &info, out, &vr), "xrCreateVulkanInstanceKHR"))
        return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

VkPhysicalDevice nvkxr_physical_device(VkInstance inst)
{
    PFN_xrGetVulkanGraphicsDevice2KHR fn = (PFN_xrGetVulkanGraphicsDevice2KHR)xr_fn("xrGetVulkanGraphicsDevice2KHR");
    XrVulkanGraphicsDeviceGetInfoKHR info = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
    VkPhysicalDevice pd = VK_NULL_HANDLE;

    if (!fn)
        return VK_NULL_HANDLE;
    info.systemId = s_xr.system;
    info.vulkanInstance = inst;
    if (!xr_ok(fn(s_xr.instance, &info, &pd), "xrGetVulkanGraphicsDevice2KHR"))
        return VK_NULL_HANDLE;
    return pd;
}

VkResult nvkxr_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out)
{
    PFN_xrCreateVulkanDeviceKHR fn = (PFN_xrCreateVulkanDeviceKHR)xr_fn("xrCreateVulkanDeviceKHR");
    XrVulkanDeviceCreateInfoKHR info = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;

    if (!fn)
        return vr;
    info.systemId = s_xr.system;
    info.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice = pd;
    info.vulkanCreateInfo = ci;
    if (!xr_ok(fn(s_xr.instance, &info, out, &vr), "xrCreateVulkanDeviceKHR"))
        return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

static int make_swapchain(Swapchain *sc, uint32_t w, uint32_t h, const char *what)
{
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    int64_t formats[64];
    uint32_t n = 0, i;
    int64_t chosen = 0;

    xrEnumerateSwapchainFormats(s_xr.session, 64, &n, formats);
    /* sRGB, in the slots' channel order: the copy into it is bit for bit. */
    for (i = 0; i < n && !chosen; i++)
        if (formats[i] == VK_FORMAT_B8G8R8A8_SRGB)
            chosen = formats[i];
    /* (R8G8B8A8_SRGB would need a channel swap in every copy: not taken) */
    if (!chosen) {
        fprintf(stderr, "[openxr] no 8-bit sRGB swapchain format (%u offered:", n);
        for (i = 0; i < n; i++)
            fprintf(stderr, " %lld", (long long)formats[i]);
        fprintf(stderr, ")\n");
        return 0;
    }
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = w;
    info.height = h;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    /* the runtime may submit on the queue it shares with the backend */
    nvk_queue_lock();
    i = xr_ok(xrCreateSwapchain(s_xr.session, &info, &sc->handle), "xrCreateSwapchain");
    nvk_queue_unlock();
    if (!i)
        return 0;
    xrEnumerateSwapchainImages(sc->handle, 0, &sc->count, NULL);
    sc->images = (XrSwapchainImageVulkan2KHR *)calloc(sc->count, sizeof *sc->images);
    for (i = 0; i < sc->count; i++)
        sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    xrEnumerateSwapchainImages(sc->handle, sc->count, &sc->count, (XrSwapchainImageBaseHeader *)sc->images);
    sc->w = w;
    sc->h = h;
    fprintf(stderr, "[openxr] %s swapchain %ux%u, format %lld, %u images\n", what, w, h,
            (long long)chosen, sc->count);
    return 1;
}

static void request_refresh_rate(void)
{
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate =
        (PFN_xrEnumerateDisplayRefreshRatesFB)xr_fn("xrEnumerateDisplayRefreshRatesFB");
    PFN_xrRequestDisplayRefreshRateFB request =
        (PFN_xrRequestDisplayRefreshRateFB)xr_fn("xrRequestDisplayRefreshRateFB");
    float rates[16], want = env_float("RECOMP_XR_REFRESH", 120.0f), best = 0.0f;
    uint32_t n = 0, i;

    if (!s_xr.refresh_ext || !enumerate || !request
            || !XR_SUCCEEDED(enumerate(s_xr.session, 16, &n, rates)))
        return;
    fprintf(stderr, "[openxr] refresh rates offered:");
    for (i = 0; i < n; i++) {
        fprintf(stderr, " %.0f", rates[i]);
        if (rates[i] <= want + 0.5f && rates[i] > best)
            best = rates[i];
    }
    fprintf(stderr, " Hz\n");
    if (best > 0.0f && xr_ok(request(s_xr.session, best), "xrRequestDisplayRefreshRateFB"))
        fprintf(stderr, "[openxr] asked for %.0f Hz\n", best);
}

static void poll_events(void)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };

    while (xrPollEvent(s_xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            int was_focused = s_xr.focused;
            s_xr.state = sc->state;
            fprintf(stderr, "[openxr] session state %d\n", (int)sc->state);
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                int ok;
                nvk_queue_lock();
                ok = xr_ok(xrBeginSession(s_xr.session, &bi), "xrBeginSession");
                nvk_queue_unlock();
                if (ok) {
                    s_xr.running = 1;
                    request_refresh_rate();
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                nvk_queue_lock();
                xrEndSession(s_xr.session);
                nvk_queue_unlock();
                s_xr.running = 0;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                s_xr.running = 0;
                fprintf(stderr, "[openxr] the runtime ended the session; exiting\n");
                fflush(stderr);
                exit(0);
            }
            s_xr.focused = sc->state == XR_SESSION_STATE_FOCUSED;
            if (s_xr.focused && !was_focused)
                s_xr.screen_placed = 0;     /* in front again on focus */
        } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const XrEventDataDisplayRefreshRateChangedFB *rc = (const XrEventDataDisplayRefreshRateChangedFB *)&ev;
            fprintf(stderr, "[openxr] refresh rate %.0f -> %.0f Hz\n", rc->fromDisplayRefreshRate,
                    rc->toDisplayRefreshRate);
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

/* The screen straight ahead of where the head points (yaw only), at eye
 * height; once the head is first tracked, on focus, and on a long press of
 * View. */
static void place_screen(XrTime time)
{
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    XrQuaternionf q;
    float yaw;

    if (!s_xr.view || !XR_SUCCEEDED(xrLocateSpace(s_xr.view, s_xr.local, time, &loc)))
        return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
            || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return;
    q = loc.pose.orientation;
    yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x));
    s_xr.screen_pose.orientation.x = 0.0f;
    s_xr.screen_pose.orientation.y = sinf(yaw * 0.5f);
    s_xr.screen_pose.orientation.z = 0.0f;
    s_xr.screen_pose.orientation.w = cosf(yaw * 0.5f);
    s_xr.screen_pose.position.x = loc.pose.position.x - sinf(yaw) * s_xr.distance;
    s_xr.screen_pose.position.y = loc.pose.position.y;
    s_xr.screen_pose.position.z = loc.pose.position.z - cosf(yaw) * s_xr.distance;
    s_xr.screen_placed = 1;
    /* the game camera follows from the eyes' own views (frame) */
    s_xr.anchor_pending = 1;
}

/* ── The picture handoff ──────────────────────────────────────────────── */

int nvkxr_presenting(void)
{
    return s_xr.started;
}

/* A slot the headset thread is not reading and that holds no newer picture
 * than the one it shows: there is always one of three. */
void *nvkxr_back_slot(uint32_t w, uint32_t h)
{
    struct nvk_image *s;
    int k;

    if (!s_xr.started || !w || !h)
        return NULL;
    pthread_mutex_lock(&s_xr.slot_lock);
    for (k = 0; k < SLOTS; k++)
        if (k != s_xr.front && k != s_xr.ready)
            break;
    s_xr.back = k;
    s_xr.stereo[k] = 0;
    pthread_mutex_unlock(&s_xr.slot_lock);
    s = &s_xr.slot[k];
    if (!s->img || s->w != w || s->h != h) {
        if (s->img) {
            /* nothing reads it now (not front, not ready), but an earlier
             * copy of it may still be on the queue */
            VkResult r = VK_SUCCESS;
            int out = nvk_wait_queue(NVK_WAIT_SLOT, "headset slot resize", s_xr.resize_fence, &r);
            if (out != NVK_WAIT_OK) {
                nvk_wait_fatal(out, r, "headset slot resize");
                return NULL;
            }
            nvk_image_free(s);
        }
        if (!nvk_image_new(s, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
            return NULL;
    }
    return s;
}

static void quat_conj_rotate(const XrQuaternionf *q, const float v[3], float out[3])
{
    /* v rotated by conj(q) */
    float x = -q->x, y = -q->y, z = -q->z, w = q->w;
    float tx = 2.0f * (y * v[2] - z * v[1]);
    float ty = 2.0f * (z * v[0] - x * v[2]);
    float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

static void fake_views(XrView v[2])
{
    /* RECOMP_XR_FAKE_YAW (degrees, left positive): a head turned, for
     * checking what the title culls outside its own view */
    static float yaw = -1000.0f;
    int e;
    if (yaw < -999.0f)
        yaw = env_float("RECOMP_XR_FAKE_YAW", 0.0f) * 0.01745329f;
    for (e = 0; e < 2; e++) {
        static float half = -1.0f;
        float x;
        if (half < 0.0f)   /* RECOMP_XR_FAKE_IPD (m): exaggerated, to see what does not follow */
            half = 0.5f * env_float("RECOMP_XR_FAKE_IPD", 0.064f);
        x = e ? half : -half;
        memset(&v[e], 0, sizeof v[e]);
        v[e].type = XR_TYPE_VIEW;
        v[e].pose.orientation.y = sinf(yaw * 0.5f);
        v[e].pose.orientation.w = cosf(yaw * 0.5f);
        v[e].pose.position.x = x * cosf(yaw);
        v[e].pose.position.z = -x * sinf(yaw);
        v[e].fov.angleLeft = -0.7853982f;
        v[e].fov.angleRight = 0.7853982f;
        v[e].fov.angleUp = 0.7853982f;
        v[e].fov.angleDown = -0.7853982f;
    }
}

int nvkxr_back_eyes(uint32_t w, uint32_t h, void *img[2], nvkxr_eye eyes[2], float *scale)
{
    XrView v[2];
    XrPosef a;
    int k = s_xr.back, e;

    if (!s_xr.started || k < 0 || !w || !h)
        return 0;
    pthread_mutex_lock(&s_xr.view_lock);
    if (s_xr.latest_valid && !s_xr.fake_views) {
        v[0] = s_xr.latest[0];
        v[1] = s_xr.latest[1];
        a = s_xr.anchor;
    } else if (s_xr.fake_views) {
        fake_views(v);
        memset(&a, 0, sizeof a);
        a.orientation.w = 1.0f;
    } else {
        pthread_mutex_unlock(&s_xr.view_lock);
        return 0;
    }
    pthread_mutex_unlock(&s_xr.view_lock);
    for (e = 0; e < 2; e++) {
        struct nvk_image *im = &s_xr.eye_img[k][e];
        float d[3], q[4];
        XrQuaternionf ca = a.orientation, vq = v[e].pose.orientation;
        if (!im->img || im->w != w || im->h != h) {
            if (im->img) {
                VkResult r = VK_SUCCESS;
                int out = nvk_wait_queue(NVK_WAIT_SLOT, "headset eye resize", s_xr.resize_fence, &r);
                if (out != NVK_WAIT_OK) {
                    nvk_wait_fatal(out, r, "headset eye resize");
                    return 0;
                }
                nvk_image_free(im);
            }
            if (!nvk_image_new(im, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
                memset(im, 0, sizeof *im);
                return 0;
            }
        }
        img[e] = im;
        s_xr.eye_view[k][e] = v[e];
        d[0] = v[e].pose.position.x - a.position.x;
        d[1] = v[e].pose.position.y - a.position.y;
        d[2] = v[e].pose.position.z - a.position.z;
        quat_conj_rotate(&ca, d, eyes[e].pos);
        /* conj(a) * v */
        q[0] = ca.w * vq.x - ca.x * vq.w - ca.y * vq.z + ca.z * vq.y;
        q[1] = ca.w * vq.y + ca.x * vq.z - ca.y * vq.w - ca.z * vq.x;
        q[2] = ca.w * vq.z - ca.x * vq.y + ca.y * vq.x - ca.z * vq.w;
        q[3] = ca.w * vq.w + ca.x * vq.x + ca.y * vq.y + ca.z * vq.z;
        memcpy(eyes[e].quat, q, sizeof q);
        eyes[e].tan[0] = tanf(v[e].fov.angleLeft);
        eyes[e].tan[1] = tanf(v[e].fov.angleRight);
        eyes[e].tan[2] = tanf(v[e].fov.angleUp);
        eyes[e].tan[3] = tanf(v[e].fov.angleDown);
    }
    *scale = s_xr.world_scale;
    return 1;
}

void nvkxr_eye_size(uint32_t *w, uint32_t *h)
{
    static float k = -1.0f;
    if (k < 0.0f)
        k = env_float("RECOMP_XR_EYE_SCALE", 1.0f);
    if (k > 0.0f && s_xr.config_views[0].recommendedImageRectWidth) {
        *w = (uint32_t)((float)s_xr.config_views[0].recommendedImageRectWidth * k + 0.5f);
        *h = (uint32_t)((float)s_xr.config_views[0].recommendedImageRectHeight * k + 0.5f);
    }
}

void *nvkxr_back_hud(uint32_t w, uint32_t h)
{
    struct nvk_image *im;
    int k = s_xr.back;
    if (!s_xr.started || k < 0 || !w || !h)
        return NULL;
    im = &s_xr.hud_img[k];
    if (!im->img || im->w != w || im->h != h) {
        if (im->img) {
            VkResult r = VK_SUCCESS;
            int out = nvk_wait_queue(NVK_WAIT_SLOT, "headset HUD resize", s_xr.resize_fence, &r);
            if (out != NVK_WAIT_OK) {
                nvk_wait_fatal(out, r, "headset HUD resize");
                return NULL;
            }
            nvk_image_free(im);
        }
        if (!nvk_image_new(im, VK_FORMAT_B8G8R8A8_UNORM, w, h, 1,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)) {
            memset(im, 0, sizeof *im);
            return NULL;
        }
    }
    return im;
}

void nvkxr_back_stereo(int flags)
{
    if (s_xr.back >= 0)
        s_xr.stereo[s_xr.back] = flags;
}

void nvkxr_publish(void)
{
    pthread_mutex_lock(&s_xr.slot_lock);
    s_xr.ready = s_xr.back;
    s_xr.back = -1;
    s_xr.published++;
    pthread_mutex_unlock(&s_xr.slot_lock);
}

/* ── The headset thread ───────────────────────────────────────────────── */

static int s_cmd_pending;   /* the command buffer's last submit not yet waited for */

static int cmd_wait(const char *what)
{
    VkResult r = VK_SUCCESS;
    int out;
    if (!s_cmd_pending)
        return 1;
    s_cmd_pending = 0;
    out = nvk_wait_fence(NVK_WAIT_PRESENT, what, s_xr.fence, &r);
    if (out != NVK_WAIT_OK) {
        nvk_wait_fatal(out, r, what);
        return 0;
    }
    return 1;
}

static int begin_cmd(void)
{
    VkCommandBufferBeginInfo bi;
    if (!cmd_wait("headset command buffer"))
        return 0;
    vkResetCommandBuffer(s_xr.cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(s_xr.cmd, &bi) == VK_SUCCESS;
}

/* Submit the command buffer, and with `wait` wait for it. Without, the
 * swapchain image is released right after: the runtime takes it on the same
 * queue, after the copy (XR_KHR_vulkan_enable2: the application's work
 * submitted before the release is complete before the runtime reads it);
 * a CPU wait for 2160x2160 eye copies made the headset miss refreshes. The
 * next begin_cmd waits for the fence before the buffer is used again. */
static int submit_cmd(const char *what, int wait)
{
    VkSubmitInfo si;
    VkResult r;
    int out;

    if (vkEndCommandBuffer(s_xr.cmd) != VK_SUCCESS)
        return 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_xr.cmd;
    vkResetFences(s_xr.core->dev, 1, &s_xr.fence);
    nvk_queue_lock();
    r = vkQueueSubmit(s_xr.core->queue, 1, &si, s_xr.fence);
    if (r == VK_SUCCESS)
        nvk_submit_note(what);
    nvk_queue_unlock();
    if (r != VK_SUCCESS) {
        nvk_device_lost(what);
        return 0;
    }
    s_cmd_pending = 1;
    (void)out;
    return wait ? cmd_wait(what) : 1;
}

static int acquire(Swapchain *sc, uint32_t *index)
{
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    int ok;

    wi.timeout = XR_INFINITE_DURATION;
    nvk_queue_lock();
    ok = xr_ok(xrAcquireSwapchainImage(sc->handle, &ai, index), "acquire")
         && xr_ok(xrWaitSwapchainImage(sc->handle, &wi), "wait");
    nvk_queue_unlock();
    return ok;
}

static void release(Swapchain *sc)
{
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    nvk_queue_lock();
    xrReleaseSwapchainImage(sc->handle, &ri);
    nvk_queue_unlock();
}

/* The eyes' images: black, or with RECOMP_XR_TEST_EYES=1 the left red and
 * the right blue. Drawn once; the compositor keeps showing the last
 * released image. */
static void draw_eyes(void)
{
    int test = getenv("RECOMP_XR_TEST_EYES") && *getenv("RECOMP_XR_TEST_EYES") == '1';
    int e;

    for (e = 0; e < 2; e++) {
        Swapchain *sc = &s_xr.eye[e];
        VkClearColorValue c;
        VkImageSubresourceRange range;
        uint32_t index;
        VkImage img;

        if (!acquire(sc, &index))
            return;
        img = sc->images[index].image;
        memset(&c, 0, sizeof c);
        c.float32[3] = 1.0f;
        if (test)
            c.float32[e ? 2 : 0] = 1.0f;
        memset(&range, 0, sizeof range);
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        if (begin_cmd()) {
            nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(s_xr.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
            nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            submit_cmd("headset eyes clear", 0);
        }
        release(sc);
    }
    s_xr.eyes_drawn = 1;
}

/* The newest picture onto the screen's swapchain: its shown rows, bit for
 * bit. */
static int show_picture(int k)
{
    struct nvk_image *s = &s_xr.slot[k];
    uint32_t y0 = (uint32_t)((uint64_t)s_xr.crop0 * s->h / 480u);
    uint32_t y1 = (uint32_t)((uint64_t)s_xr.crop1 * s->h / 480u);
    uint32_t rows = y1 > y0 ? y1 - y0 : s->h, index;
    VkImageCopy ic;
    VkImage img;
    int dump = 0, ok = 0;

    if (s_xr.dump && now_ns() - s_xr.dump_last >= 5000000000ull && s_xr.dump_n < 100) {
        VkDeviceSize need = (VkDeviceSize)s->w * rows * 4;
        if (s_xr.dump_buf.buf && s_xr.dump_buf.size < need)
            nvk_buffer_free(&s_xr.dump_buf);
        dump = s_xr.dump_buf.buf
               || nvk_buffer_new(&s_xr.dump_buf, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (s_xr.screen.handle && (s_xr.screen.w != s->w || s_xr.screen.h != rows)) {
        nvk_queue_lock();
        xrDestroySwapchain(s_xr.screen.handle);
        nvk_queue_unlock();
        free(s_xr.screen.images);
        memset(&s_xr.screen, 0, sizeof s_xr.screen);
    }
    if (!s_xr.screen.handle && !make_swapchain(&s_xr.screen, s->w, rows, "screen"))
        return 0;
    if (!acquire(&s_xr.screen, &index))
        return 0;
    img = s_xr.screen.images[index].image;
    if (begin_cmd()) {
        /* after the backend's copy into the slot (submitted before publish) */
        nvk_image_barrier(s_xr.cmd, s->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_GENERAL);
        nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&ic, 0, sizeof ic);
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.srcOffset.y = (int32_t)y0;
        ic.extent.width = s->w;
        ic.extent.height = rows;
        ic.extent.depth = 1;
        /* the slot is GENERAL (the backend's copy left it so) */
        vkCmdCopyImage(s_xr.cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &ic);
        if (dump) {
            /* diagnostics only: a readback, every 5 s */
            VkBufferImageCopy bc;
            memset(&bc, 0, sizeof bc);
            bc.imageSubresource = ic.srcSubresource;
            bc.imageOffset.y = (int32_t)y0;
            bc.imageExtent = ic.extent;
            vkCmdCopyImageToBuffer(s_xr.cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, s_xr.dump_buf.buf, 1, &bc);
        }
        nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ok = submit_cmd("headset screen copy", dump);
        if (ok && dump && s_xr.dump_buf.map) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s%03d.ppm", s_xr.dump, s_xr.dump_n++);
            f = fopen(path, "wb");
            if (f) {
                const uint8_t *px = (const uint8_t *)s_xr.dump_buf.map;
                uint8_t *row = (uint8_t *)malloc((size_t)s->w * 3);
                uint32_t x, y;
                fprintf(f, "P6\n%u %u\n255\n", s->w, rows);
                for (y = 0; row && y < rows; y++) {
                    for (x = 0; x < s->w; x++) {   /* B8G8R8A8 to RGB */
                        const uint8_t *p = px + ((size_t)y * s->w + x) * 4;
                        row[x * 3] = p[2];
                        row[x * 3 + 1] = p[1];
                        row[x * 3 + 2] = p[0];
                    }
                    fwrite(row, 1, (size_t)s->w * 3, f);
                }
                free(row);
                fclose(f);
                fprintf(stderr, "[openxr] picture dumped to %s (%ux%u)\n", path, s->w, rows);
            }
            s_xr.dump_last = now_ns();
        }
    }
    release(&s_xr.screen);
    /* the image released holds the picture only when the copy went */
    s_xr.screen_valid = ok;
    return ok;
}

static void perf_frame(XrTime predicted, uint64_t wait_ns, int fresh)
{
    uint64_t now = now_ns();

    if (s_xr.predicted && s_xr.period && predicted - s_xr.predicted > s_xr.period * 3 / 2)
        s_xr.perf_missed += (uint64_t)((predicted - s_xr.predicted + s_xr.period / 2) / s_xr.period) - 1;
    s_xr.perf_frames++;
    s_xr.perf_new += fresh;
    s_xr.perf_wait_ns += wait_ns;
    if (!s_xr.perf_t0)
        s_xr.perf_t0 = now;
    if (now - s_xr.perf_t0 >= 5000000000ull) {
        double sec = (double)(now - s_xr.perf_t0) / 1e9;
        uint64_t pub = __atomic_load_n(&s_xr.published, __ATOMIC_RELAXED);
        fprintf(stderr, "[vr-perf] %.1f fps (app period %.1f Hz), %.1f pictures/s from the game, %.1f new"
                        " pictures/s shown (%.1f in stereo), %llu missed, wait %.2f ms%s\n",
                (double)s_xr.perf_frames / sec,
                s_xr.period ? 1e9 / (double)s_xr.period : 0.0, (double)(pub - s_xr.perf_published) / sec,
                (double)s_xr.perf_new / sec, (double)s_xr.perf_stereo / sec,
                (unsigned long long)s_xr.perf_missed,
                (double)s_xr.perf_wait_ns / 1e6 / (double)s_xr.perf_frames,
                s_xr.focused ? "" : " (not focused)");
        s_xr.perf_published = pub;
        s_xr.perf_stereo = 0;
        s_xr.perf_t0 = now;
        s_xr.perf_frames = s_xr.perf_new = s_xr.perf_missed = s_xr.perf_wait_ns = 0;
    }
}

/* A stereo slot's eye images (their shown rows) onto the stereo eyes'
 * swapchains, bit for bit. */
static int show_stereo(int k)
{
    uint32_t w = s_xr.eye_img[k][0].w, h = s_xr.eye_img[k][0].h;
    uint32_t y0 = (uint32_t)((uint64_t)s_xr.crop0 * h / 480u);
    uint32_t y1 = (uint32_t)((uint64_t)s_xr.crop1 * h / 480u);
    uint32_t rows = y1 > y0 ? y1 - y0 : h, index[2];
    int e, dump = 0, ok = 0;

    if (s_xr.dump && now_ns() - s_xr.dump_last >= 5000000000ull && s_xr.dump_n < 100) {
        VkDeviceSize need = (VkDeviceSize)w * rows * 4 * 4;
        if (s_xr.dump_buf.buf && s_xr.dump_buf.size < need)
            nvk_buffer_free(&s_xr.dump_buf);
        dump = s_xr.dump_buf.buf
               || nvk_buffer_new(&s_xr.dump_buf, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    for (e = 0; e < 2; e++) {
        Swapchain *sc = &s_xr.st_eye[e];
        if (sc->handle && (sc->w != w || sc->h != rows)) {
            nvk_queue_lock();
            xrDestroySwapchain(sc->handle);
            nvk_queue_unlock();
            free(sc->images);
            memset(sc, 0, sizeof *sc);
        }
        if (!sc->handle && !make_swapchain(sc, w, rows, e ? "stereo right eye" : "stereo left eye"))
            return 0;
    }
    {
        int with_hud = (s_xr.stereo[k] & 2) != 0;
        uint32_t hw = s_xr.hud_img[k].w, hh = s_xr.hud_img[k].h;
        uint32_t hy0 = (uint32_t)((uint64_t)s_xr.hud_crop0 * hh / 480u);
        uint32_t hy1 = (uint32_t)((uint64_t)s_xr.hud_crop1 * hh / 480u);
        uint32_t hrows = hy1 > hy0 ? hy1 - hy0 : hh, hindex;
        if (with_hud && s_xr.hud.handle && (s_xr.hud.w != hw || s_xr.hud.h != hrows)) {
            nvk_queue_lock();
            xrDestroySwapchain(s_xr.hud.handle);
            nvk_queue_unlock();
            free(s_xr.hud.images);
            memset(&s_xr.hud, 0, sizeof s_xr.hud);
        }
        if (with_hud && !s_xr.hud.handle && !make_swapchain(&s_xr.hud, hw, hrows, "HUD"))
            with_hud = 0;
        if (with_hud && acquire(&s_xr.hud, &hindex)) {
            if (!begin_cmd()) {
                with_hud = 0;
            } else {
                VkImage img = s_xr.hud.images[hindex].image;
                VkImageCopy ic;
                nvk_image_barrier(s_xr.cmd, s_xr.hud_img[k].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                memset(&ic, 0, sizeof ic);
                ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.srcSubresource.layerCount = 1;
                ic.dstSubresource = ic.srcSubresource;
                ic.srcOffset.y = (int32_t)hy0;
                ic.extent.width = hw;
                ic.extent.height = hrows;
                ic.extent.depth = 1;
                vkCmdCopyImage(s_xr.cmd, s_xr.hud_img[k].img, VK_IMAGE_LAYOUT_GENERAL, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (!submit_cmd("headset HUD copy", 0))
                    with_hud = 0;
            }
            release(&s_xr.hud);
        } else {
            with_hud = 0;
        }
        s_xr.shown_hud = with_hud;
    }
    if (!acquire(&s_xr.st_eye[0], &index[0]))
        return 0;
    if (!acquire(&s_xr.st_eye[1], &index[1])) {
        release(&s_xr.st_eye[0]);
        return 0;
    }
    if (begin_cmd()) {
        ok = 1;
        for (e = 0; e < 2; e++) {
            struct nvk_image *src = &s_xr.eye_img[k][e];
            VkImage img = s_xr.st_eye[e].images[index[e]].image;
            VkImageCopy ic;
            nvk_image_barrier(s_xr.cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_GENERAL);
            nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&ic, 0, sizeof ic);
            ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ic.srcSubresource.layerCount = 1;
            ic.dstSubresource = ic.srcSubresource;
            ic.srcOffset.y = (int32_t)y0;
            ic.extent.width = w;
            ic.extent.height = rows;
            ic.extent.depth = 1;
            vkCmdCopyImage(s_xr.cmd, src->img, VK_IMAGE_LAYOUT_GENERAL, img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
            if (dump) {
                VkBufferImageCopy bc;
                memset(&bc, 0, sizeof bc);
                bc.bufferOffset = (VkDeviceSize)e * w * rows * 4;
                bc.imageSubresource = ic.srcSubresource;
                bc.imageOffset.y = (int32_t)y0;
                bc.imageExtent = ic.extent;
                vkCmdCopyImageToBuffer(s_xr.cmd, src->img, VK_IMAGE_LAYOUT_GENERAL, s_xr.dump_buf.buf, 1,
                                       &bc);
            }
            nvk_image_barrier(s_xr.cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (dump && s_xr.slot[k].img && s_xr.slot[k].w == w && s_xr.slot[k].h == h) {
            /* the frame's flat picture too, for comparison */
            VkBufferImageCopy bc;
            nvk_image_barrier(s_xr.cmd, s_xr.slot[k].img, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            memset(&bc, 0, sizeof bc);
            bc.bufferOffset = (VkDeviceSize)2 * w * rows * 4;
            bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount = 1;
            bc.imageOffset.y = (int32_t)y0;
            bc.imageExtent.width = w;
            bc.imageExtent.height = rows;
            bc.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(s_xr.cmd, s_xr.slot[k].img, VK_IMAGE_LAYOUT_GENERAL, s_xr.dump_buf.buf,
                                   1, &bc);
        }
        if (dump && (s_xr.stereo[k] & 2) && s_xr.hud_img[k].img && s_xr.hud_img[k].w == w
                && s_xr.hud_img[k].h == h) {
            VkBufferImageCopy bc;
            memset(&bc, 0, sizeof bc);
            bc.bufferOffset = (VkDeviceSize)3 * w * rows * 4;
            bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bc.imageSubresource.layerCount = 1;
            bc.imageOffset.y = (int32_t)y0;
            bc.imageExtent.width = w;
            bc.imageExtent.height = rows;
            bc.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(s_xr.cmd, s_xr.hud_img[k].img, VK_IMAGE_LAYOUT_GENERAL,
                                   s_xr.dump_buf.buf, 1, &bc);
        }
        ok = submit_cmd("headset stereo copy", dump);
        if (ok && dump && s_xr.dump_buf.map) {
            for (e = 0; e < ((s_xr.stereo[k] & 2) ? 4 : 3); e++) {
                char path[512];
                FILE *f;
                snprintf(path, sizeof path, "%s%03d-%c.%s", s_xr.dump, s_xr.dump_n, "LRMH"[e],
                         e == 3 ? "pam" : "ppm");
                f = fopen(path, "wb");
                if (f) {
                    const uint8_t *px = (const uint8_t *)s_xr.dump_buf.map + (size_t)e * w * rows * 4;
                    int ch = e == 3 ? 4 : 3;   /* the HUD with its alpha (PAM) */
                    uint8_t *row = (uint8_t *)malloc((size_t)w * 4);
                    uint32_t x, y;
                    if (ch == 4)
                        fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                                w, rows);
                    else
                        fprintf(f, "P6\n%u %u\n255\n", w, rows);
                    for (y = 0; row && y < rows; y++) {
                        for (x = 0; x < w; x++) {
                            const uint8_t *p = px + ((size_t)y * w + x) * 4;
                            row[x * ch] = p[2];
                            row[x * ch + 1] = p[1];
                            row[x * ch + 2] = p[0];
                            if (ch == 4)
                                row[x * ch + 3] = p[3];
                        }
                        fwrite(row, 1, (size_t)w * ch, f);
                    }
                    free(row);
                    fclose(f);
                }
            }
            for (e = 0; e < 2; e++) {
                const XrView *v = &s_xr.eye_view[k][e];
                fprintf(stderr, "[openxr] %s eye %c dumped to %s%03d-%c.ppm (%ux%u): at %.4f %.4f %.4f,"
                        " orientation %.4f %.4f %.4f %.4f, tangents l %.4f r %.4f u %.4f d %.4f\n",
                        (s_xr.stereo[k] & NVKXR_CINEMA) ? "cinema" : "immersive",
                        e ? 'R' : 'L', s_xr.dump, s_xr.dump_n, e ? 'R' : 'L', w, rows,
                        v->pose.position.x, v->pose.position.y, v->pose.position.z,
                        v->pose.orientation.x, v->pose.orientation.y, v->pose.orientation.z,
                        v->pose.orientation.w, tanf(v->fov.angleLeft), tanf(v->fov.angleRight),
                        tanf(v->fov.angleUp), tanf(v->fov.angleDown));
            }
            s_xr.dump_n++;
            s_xr.dump_last = now_ns();
        }
    }
    release(&s_xr.st_eye[0]);
    release(&s_xr.st_eye[1]);
    if (!ok) {
        /* the eyes' images released hold nothing: no stereo layer */
        s_xr.shown_stereo = 0;
        s_xr.shown_hud = 0;
        return 0;
    }
    s_xr.shown_view[0] = s_xr.eye_view[k][0];
    s_xr.shown_view[1] = s_xr.eye_view[k][1];
    s_xr.shown_stereo = 1;
    s_xr.shown_cinema = (s_xr.stereo[k] & NVKXR_CINEMA) != 0;
    return 1;
}

static void frame(void)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad hud = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad cine[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    const XrCompositionLayerBaseHeader *layers[4];
    uint64_t t0 = now_ns();
    uint32_t nl = 0, n = 0;
    int k = -1, fresh = 0, i, located = 0, stereo = 0;

    if (!xr_ok(xrWaitFrame(s_xr.session, &wi, &fs), "xrWaitFrame"))
        return;
    perf_frame(fs.predictedDisplayTime, now_ns() - t0, 0);
    s_xr.period = fs.predictedDisplayPeriod;
    s_xr.predicted = fs.predictedDisplayTime;
    nvk_queue_lock();
    i = xr_ok(xrBeginFrame(s_xr.session, &bi), "xrBeginFrame");
    nvk_queue_unlock();
    if (!i)
        return;

    if (sync_input()) {
        if (!s_xr.view_held_since)
            s_xr.view_held_since = (double)fs.predictedDisplayTime;
        else if ((double)fs.predictedDisplayTime - s_xr.view_held_since > 1e9) {
            s_xr.screen_placed = 0;
            s_xr.view_held_since = (double)fs.predictedDisplayTime;
        }
    } else {
        s_xr.view_held_since = 0;
    }
    if (!s_xr.screen_placed)
        place_screen(fs.predictedDisplayTime);

    /* The eyes where they will be: for this frame's layers and the
     * backend's next stereo picture. */
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = s_xr.local;
    s_xr.views[0].type = s_xr.views[1].type = XR_TYPE_VIEW;
    if (XR_SUCCEEDED(xrLocateViews(s_xr.session, &li, &vs, 2, &n, s_xr.views)) && n == 2) {
        located = 1;
        if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
                && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
                && (s_xr.anchor_pending || s_xr.latest_valid)) {
            pthread_mutex_lock(&s_xr.view_lock);
            if (s_xr.anchor_pending) {
                /* The game camera: between the eyes, facing where they
                 * face (yaw only), from the same views the eyes are drawn
                 * for, so the two always agree. */
                const XrQuaternionf *q = &s_xr.views[0].pose.orientation;
                float yaw = atan2f(2.0f * (q->w * q->y + q->x * q->z), 1.0f - 2.0f * (q->y * q->y + q->x * q->x));
                s_xr.anchor.orientation.x = 0.0f;
                s_xr.anchor.orientation.y = sinf(yaw * 0.5f);
                s_xr.anchor.orientation.z = 0.0f;
                s_xr.anchor.orientation.w = cosf(yaw * 0.5f);
                s_xr.anchor.position.x = 0.5f * (s_xr.views[0].pose.position.x + s_xr.views[1].pose.position.x);
                s_xr.anchor.position.y = 0.5f * (s_xr.views[0].pose.position.y + s_xr.views[1].pose.position.y);
                s_xr.anchor.position.z = 0.5f * (s_xr.views[0].pose.position.z + s_xr.views[1].pose.position.z);
                s_xr.anchor_pending = 0;
                fprintf(stderr, "[openxr] game camera at %.3f %.3f %.3f, yaw %.1f degrees\n",
                        s_xr.anchor.position.x, s_xr.anchor.position.y, s_xr.anchor.position.z,
                        yaw * 57.29578f);
            }
            s_xr.latest[0] = s_xr.views[0];
            s_xr.latest[1] = s_xr.views[1];
            s_xr.latest_valid = 1;
            pthread_mutex_unlock(&s_xr.view_lock);
        }
    }

    /* the newest picture, if there is one the screen has not shown (also in
     * standby, so the screen holds the newest when it wakes) */
    pthread_mutex_lock(&s_xr.slot_lock);
    if (s_xr.ready >= 0) {
        s_xr.front = s_xr.ready;
        s_xr.ready = -1;
        k = s_xr.front;
    }
    pthread_mutex_unlock(&s_xr.slot_lock);
    if (k >= 0) {
        if (s_xr.stereo[k] && show_stereo(k)) {
            fresh = 1;
            s_xr.perf_stereo++;
        } else if (show_picture(k)) {
            fresh = 1;
            s_xr.shown_stereo = 0;
        }
    }
    stereo = s_xr.shown_stereo && s_xr.st_eye[0].handle;

    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (fs.shouldRender) {
        int cinema3d = stereo && s_xr.shown_cinema;
        if ((!stereo || cinema3d) && !s_xr.eyes_drawn)
            draw_eyes();
        if (cinema3d && located && s_xr.eyes_drawn) {
            /* the black eyes behind the 3D cinema screen */
            for (i = 0; i < 2; i++) {
                memset(&pv[i], 0, sizeof pv[i]);
                pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[i].pose = s_xr.views[i].pose;
                pv[i].fov = s_xr.views[i].fov;
                pv[i].subImage.swapchain = s_xr.eye[i].handle;
                pv[i].subImage.imageRect.extent.width = (int32_t)s_xr.eye[i].w;
                pv[i].subImage.imageRect.extent.height = (int32_t)s_xr.eye[i].h;
            }
            proj.space = s_xr.local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        if (cinema3d && s_xr.screen_placed) {
            /* each eye its own picture on the screen */
            for (i = 0; i < 2; i++) {
                cine[i].space = s_xr.local;
                cine[i].eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
                cine[i].subImage.swapchain = s_xr.st_eye[i].handle;
                cine[i].subImage.imageRect.extent.width = (int32_t)s_xr.st_eye[i].w;
                cine[i].subImage.imageRect.extent.height = (int32_t)s_xr.st_eye[i].h;
                cine[i].pose = s_xr.screen_pose;
                cine[i].size.width = s_xr.width;
                cine[i].size.height = s_xr.width / s_xr.aspect * (float)(s_xr.crop1 - s_xr.crop0) / 480.0f;
                layers[nl++] = (const XrCompositionLayerBaseHeader *)&cine[i];
            }
        } else if (stereo || (located && s_xr.eyes_drawn)) {
            for (i = 0; i < 2; i++) {
                const Swapchain *sc = stereo ? &s_xr.st_eye[i] : &s_xr.eye[i];
                const XrView *v = stereo ? &s_xr.shown_view[i] : &s_xr.views[i];
                memset(&pv[i], 0, sizeof pv[i]);
                pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                /* the poses the eyes were drawn for */
                pv[i].pose = v->pose;
                pv[i].fov = v->fov;
                pv[i].subImage.swapchain = sc->handle;
                pv[i].subImage.imageRect.extent.width = (int32_t)sc->w;
                pv[i].subImage.imageRect.extent.height = (int32_t)sc->h;
            }
            proj.space = s_xr.local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        if (stereo && s_xr.shown_hud && s_xr.hud.handle && located) {
            /* The HUD panel ahead of the head, following its yaw and pitch,
             * never its roll. */
            const XrQuaternionf *q = &s_xr.views[0].pose.orientation;
            float fx = -2.0f * (q->x * q->z + q->w * q->y);
            float fy = -2.0f * (q->y * q->z - q->w * q->x);
            float fz = -(1.0f - 2.0f * (q->x * q->x + q->y * q->y));
            float yaw = atan2f(-fx, -fz), pitch = asinf(fy < -1.0f ? -1.0f : fy > 1.0f ? 1.0f : fy);
            float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
            float d = s_xr.hud_distance;
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hud.space = s_xr.local;
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain = s_xr.hud.handle;
            hud.subImage.imageRect.extent.width = (int32_t)s_xr.hud.w;
            hud.subImage.imageRect.extent.height = (int32_t)s_xr.hud.h;
            /* yaw about y, then pitch about x */
            hud.pose.orientation.x = cy * sp;
            hud.pose.orientation.y = sy * cp;
            hud.pose.orientation.z = -sy * sp;
            hud.pose.orientation.w = cy * cp;
            hud.pose.position.x = 0.5f * (s_xr.views[0].pose.position.x + s_xr.views[1].pose.position.x)
                                  - d * sinf(yaw) * cosf(pitch);
            hud.pose.position.y = 0.5f * (s_xr.views[0].pose.position.y + s_xr.views[1].pose.position.y)
                                  + d * sinf(pitch);
            hud.pose.position.z = 0.5f * (s_xr.views[0].pose.position.z + s_xr.views[1].pose.position.z)
                                  - d * cosf(yaw) * cosf(pitch);
            hud.size.width = s_xr.hud_width;
            hud.size.height = s_xr.hud_width / s_xr.aspect * (float)(s_xr.hud_crop1 - s_xr.hud_crop0) / 480.0f;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&hud;
        }
        if (!stereo && s_xr.screen.handle && s_xr.screen_valid && s_xr.screen_placed) {
            quad.space = s_xr.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s_xr.screen.handle;
            quad.subImage.imageRect.extent.width = (int32_t)s_xr.screen.w;
            quad.subImage.imageRect.extent.height = (int32_t)s_xr.screen.h;
            quad.pose = s_xr.screen_pose;
            quad.size.width = s_xr.width;
            /* the picture at its display aspect (anamorphic 16:9 too),
             * whatever the render scale */
            quad.size.height = s_xr.width / s_xr.aspect * (float)(s_xr.crop1 - s_xr.crop0) / 480.0f;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
        }
    }
    s_xr.perf_new += fresh;
    ei.layerCount = nl;
    ei.layers = nl ? layers : NULL;
    nvk_queue_lock();
    xr_ok(xrEndFrame(s_xr.session, &ei), "xrEndFrame");
    nvk_queue_unlock();
}

static void *headset_thread(void *arg)
{
    (void)arg;
    while (!s_xr.quit) {
        poll_events();
        if (!s_xr.running) {
            struct timespec ts = { 0, 10000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        frame();
    }
    return NULL;
}

void nvkxr_start(const struct nvk_core *core)
{
    XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cai;
    VkFenceCreateInfo fci;
    uint32_t nv = 0, i;
    pthread_t th;

    if (!s_xr.prepared || s_xr.started)
        return;
    s_xr.core = core;
    binding.instance = core->inst;
    binding.physicalDevice = core->phys;
    binding.device = core->dev;
    binding.queueFamilyIndex = core->qfam;
    binding.queueIndex = 0;
    sci.next = &binding;
    sci.systemId = s_xr.system;
    if (!xr_ok(xrCreateSession(s_xr.instance, &sci, &s_xr.session), "xrCreateSession"))
        return;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!xr_ok(xrCreateReferenceSpace(s_xr.session, &rsci, &s_xr.local), "LOCAL space"))
        return;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr_ok(xrCreateReferenceSpace(s_xr.session, &rsci, &s_xr.view), "VIEW space");
    create_actions();
    s_pad_backend = xbox_input_host_backend;
    s_pad_backend.init = pad_init;
    s_pad_backend.get_state = pad_get_state;
    s_pad_backend.set_state = pad_set_state;
    s_pad_backend.shutdown = pad_shutdown;
    xbox_InputSetBackend(&s_pad_backend);

    for (i = 0; i < 2; i++)
        s_xr.config_views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(s_xr.instance, s_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      2, &nv, s_xr.config_views);
    for (i = 0; i < 2; i++)
        if (!make_swapchain(&s_xr.eye[i], s_xr.config_views[i].recommendedImageRectWidth,
                            s_xr.config_views[i].recommendedImageRectHeight, i ? "right eye" : "left eye"))
            return;

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = core->qfam;
    vkCreateCommandPool(core->dev, &pci, NULL, &s_xr.pool);
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = s_xr.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(core->dev, &cai, &s_xr.cmd);
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(core->dev, &fci, NULL, &s_xr.fence);
    vkCreateFence(core->dev, &fci, NULL, &s_xr.resize_fence);

    s_xr.dump = getenv("RECOMP_XR_DUMP") && *getenv("RECOMP_XR_DUMP") ? getenv("RECOMP_XR_DUMP") : NULL;
    s_xr.world_scale = env_float("RECOMP_XR_WORLD_SCALE", 1.0f);
    if (!(s_xr.world_scale > 0.0f))
        s_xr.world_scale = 1.0f;
    s_xr.fake_views = getenv("RECOMP_XR_FAKE_VIEWS") && *getenv("RECOMP_XR_FAKE_VIEWS") == '1';
    s_xr.hud_distance = env_float("RECOMP_XR_HUD_DISTANCE", 1.75f);
    s_xr.hud_width = 2.0f * s_xr.hud_distance
                     * tanf(0.5f * env_float("RECOMP_XR_HUD_ANGLE", 40.0f) * 0.01745329f);
    s_xr.distance = env_float("RECOMP_XR_SCREEN_DISTANCE", 2.2f);
    /* the configured display aspect (16:9 for a 16:9 title's 640x480) */
    s_xr.aspect = (float)nv2a_present_width(640, 480) / 480.0f;
    if (!(s_xr.aspect > 0.5f && s_xr.aspect < 4.0f))
        s_xr.aspect = 4.0f / 3.0f;
    s_xr.width = env_float("RECOMP_XR_SCREEN_WIDTH", 2.6f);
    s_xr.crop0 = 0;
    s_xr.crop1 = 480;
    {
        const char *e = getenv("RECOMP_XR_CROP_Y");
        int a, b;
        if (e && sscanf(e, "%d,%d", &a, &b) == 2 && a >= 0 && b > a && b <= 480) {
            s_xr.crop0 = (uint32_t)a;
            s_xr.crop1 = (uint32_t)b;
        }
    }
    s_xr.hud_crop0 = s_xr.crop0;
    s_xr.hud_crop1 = s_xr.crop1;
    {
        const char *e = getenv("RECOMP_XR_HUD_CROP_Y");
        int a, b;
        if (e && sscanf(e, "%d,%d", &a, &b) == 2 && a >= 0 && b > a && b <= 480) {
            s_xr.hud_crop0 = (uint32_t)a;
            s_xr.hud_crop1 = (uint32_t)b;
        }
    }
    if (pthread_create(&th, NULL, headset_thread, NULL) != 0) {
        /* no headset thread: the window keeps the pictures */
        fprintf(stderr, "[openxr] the headset thread could not start: no headset\n");
        return;
    }
    pthread_detach(th);
    s_xr.started = 1;
    fprintf(stderr, "[openxr] session made on %s, queue family %u; eyes %ux%u; headset thread started\n",
            core->name, core->qfam, s_xr.config_views[0].recommendedImageRectWidth,
            s_xr.config_views[0].recommendedImageRectHeight);
}
