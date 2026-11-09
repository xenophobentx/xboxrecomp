/* The VR settings' ids (the VR page), included inside the xs_id enum of xbox_settings.h after the SimHub ones. Own files, like the wheel's and SimHub's: the rows are in xbox_settings_vr_set.inc, the labels in overlay_vr_meta.inc. */
    XS_VR_IPD,              /* the VR page: the headset picture's options (the game's seatcam.c reads them; the headset itself starts with --vr) */
    XS_VR_NEAR,
    XS_VR_FOV,
    XS_VR_MENU_ASPECT,
    XS_VR_HIDE_DRIVER,
    XS_VR_BOOSTFX,
    XS_VR_KEEP_FX,
    XS_VR_ASPECT_X,
    XS_VR_ASPECT_Y,
    XS_VR_SCREEN_X,
    XS_VR_SCREEN_Y,
    XS_VR_SCALE,            /* the headset picture's quality, used in place of the Graphics page's while in VR */
    XS_VR_AA,
    XS_VR_SHARPEN,
    XS_VR_LOD_BIAS,
    XS_VR_API,
