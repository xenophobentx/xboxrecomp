/**
 * OpenXR on the Vulkan backend's device (Linux; the Steam Frame).
 *
 * RECOMP_XR=1 turns it on. The OpenXR runtime then makes the Vulkan instance
 * and device (XR_KHR_vulkan_enable2) for nv2a_vk_core.c, on the GPU the
 * headset uses, and once the device is up a headset thread of its own runs
 * the frame loop: it waits for each display frame, shows the newest picture
 * the backend finished and ends the frame. The backend hands pictures over
 * as it does to the window (xbox_FramebufferWindowBackTexture, then
 * PublishTexture): fb_present_sdl.c sends those here while the headset is
 * on. The headset thread submits on the one queue under nvk_queue_lock, as
 * the window's presents do.
 *
 * A flat picture goes on a screen in front of the player ("cinema"), at
 * the configured display aspect, over a projection layer: black, or a
 * colour per eye with RECOMP_XR_TEST_EYES=1 (left red, right blue). The
 * Frame controllers drive the title's first pad while the session has focus
 * (an input backend over the host's).
 *
 * Stereo: nv2a_pb_vulkan.c draws the picture again for each eye into the
 * slot's eye images (nvkxr_back_eyes, at nvkxr_eye_size); such a slot is
 * shown as a projection layer at the poses it was drawn for, its rows
 * RECOMP_XR_CROP_Y, or, marked NVKXR_CINEMA (a frame the title says is
 * not the player's own view, xbox_vr_scene), as one quad per eye on the
 * cinema screen. The game camera is the anchor: the eyes' midpoint at the
 * last recentre (yaw only). RECOMP_XR_WORLD_SCALE (1) game units per
 * metre. For unattended checks of the geometry: RECOMP_XR_FAKE_VIEWS=1
 * draws for fixed eyes instead (64 mm apart, 90 degrees, the anchor at
 * their middle; RECOMP_XR_FAKE_IPD, RECOMP_XR_FAKE_YAW), RECOMP_XR_DUMP
 * writes the eyes, the flat picture and the HUD as image files now and
 * then, RECOMP_XR_STEREO_TEST and RECOMP_XR_STEREO_LOG (nv2a_pb_vulkan.c).
 * The eyes' anti-aliasing: RECOMP_XR_FXAA (on), RECOMP_XR_MSAA (off); their
 * size: RECOMP_XR_EYE_SCALE (1, the runtime's recommended size).
 *
 * The HUD (stereo frames): its own image with derived alpha, on a panel
 * that follows the head's yaw and pitch, never its roll: RECOMP_XR_HUD_DISTANCE
 * (1.75 m), RECOMP_XR_HUD_ANGLE (40 degrees wide), RECOMP_XR_HUD_CROP_Y
 * (rows, as RECOMP_XR_CROP_Y; default that).
 *
 * Settings: RECOMP_XR_SCREEN_DISTANCE, RECOMP_XR_SCREEN_WIDTH (metres, 2.2
 * and 2.6), RECOMP_XR_CROP_Y="y0,y1" (the rows the title draws, of 480;
 * Burnout 3 letterboxes 16 at the top and bottom), RECOMP_XR_REFRESH (Hz to
 * ask for, 120). Hold View for a second to recentre.
 */
#ifndef XBOXRECOMP_NV2A_VK_XR_H
#define XBOXRECOMP_NV2A_VK_XR_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvk_core;

/* RECOMP_XR=1 asked for it, and (after nvkxr_prepare) the runtime is there. */
int nvkxr_wanted(void);
/* Before the Vulkan instance: the OpenXR instance and system. 0 when the
 * headset is not wanted or not there (the backend then runs as without). */
int nvkxr_prepare(void);
/* The Vulkan instance and device, made by the runtime. */
VkResult nvkxr_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkPhysicalDevice nvkxr_physical_device(VkInstance inst);
VkResult nvkxr_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out);
/* The device is up: the session and the headset thread. */
void nvkxr_start(const struct nvk_core *core);

/* The window's side of the picture handoff, while the headset is on. */
int nvkxr_presenting(void);
void *nvkxr_back_slot(uint32_t w, uint32_t h);   /* struct nvk_image * */
void nvkxr_publish(void);

/* Stereo (stage 2): an eye's view relative to the anchor (the game camera:
 * OpenXR axes, metres) and its field of view as tangents (left, right, up,
 * down; left and down negative). */
typedef struct nvkxr_eye {
    float pos[3], quat[4], tan[4];
} nvkxr_eye;
/* Between nvkxr_back_slot and nvkxr_publish: the back slot's two eye images
 * (w x h, B8G8R8A8_UNORM, struct nvk_image *; the caller leaves them
 * GENERAL), the eyes to draw them for and the world scale (metres per game
 * unit is 1/scale). The views are kept with the slot: the headset shows the
 * images at the poses they were drawn for. 0 when there are no views yet. */
int nvkxr_back_eyes(uint32_t w, uint32_t h, void *img[2], nvkxr_eye eyes[2], float *scale);
/* The size to draw the eyes at: the runtime's recommended eye size times
 * RECOMP_XR_EYE_SCALE (1; 0 keeps the frame's own size, *w x *h). */
void nvkxr_eye_size(uint32_t *w, uint32_t *h);
/* The back slot's HUD image (w x h, as the eye images): premultiplied
 * alpha, shown on a panel when the slot is stereo with NVKXR_HUD. */
void *nvkxr_back_hud(uint32_t w, uint32_t h);
/* The eye images were drawn (NVKXR_EYES), and the HUD (NVKXR_HUD): the slot
 * is shown in stereo. */
#define NVKXR_EYES   1
#define NVKXR_HUD    2
#define NVKXR_CINEMA 4   /* fixed eyes for the cinema screen, one quad each */
void nvkxr_back_stereo(int flags);

#ifdef __cplusplus
}
#endif

#endif
