/* overlay_vr.inc -- included in overlay_ui.cpp after overlay_simhub.inc: the VR page, the headset picture's options (the settings
 * are in xbox_settings_vr_*.inc; the game's seatcam.c reads them, the toolkit's oxr.c the menu screen's shape). The headset itself
 * starts with the --vr option, not from here. */
static int vr_rows(int page, Row *r)
{
    int n = 0;
#define VINFO(t) r[n++] = Row{ R_INFO, XS_COUNT, 0, (t) }
#define VSET(i)  r[n++] = Row{ R_SETTING, (i), 0, NULL }

    (void)page;
    VINFO("The headset picture. VR starts with the --vr option (a launcher); these apply after a restart.");
    VSET(XS_VR_API);
    VSET(XS_VR_IPD);
    VSET(XS_VR_NEAR);
    VSET(XS_VR_FOV);
    VINFO("The picture quality in the headset, used instead of the Graphics page's while in VR (restart to apply).");
    VSET(XS_VR_SCALE);
    VSET(XS_VR_AA);
    VSET(XS_VR_SHARPEN);
    VSET(XS_VR_LOD_BIAS);
    VINFO("The car view: picture size, on top of the display aspect and ultrawide settings; 0 and 0 is as the game draws it. These show at once.");
    VSET(XS_VR_ASPECT_X);
    VSET(XS_VR_ASPECT_Y);
    VINFO("The flat screen, the cinema view of the menus, crash views and replays: its shape, then a width and height adjust (0 and 0: as set).");
    VSET(XS_VR_MENU_ASPECT);
    VSET(XS_VR_SCREEN_X);
    VSET(XS_VR_SCREEN_Y);
    VSET(XS_VR_HIDE_DRIVER);
    VSET(XS_VR_BOOSTFX);
    VSET(XS_VR_KEEP_FX);
    r[n++] = Row{ R_ACTION, XS_COUNT, A_RESET_PAGE, "Reset this page's settings to the defaults" };
    return n;
#undef VSET
#undef VINFO
}
