/* xbox_settings_ours.inc -- the VR page's code in xbox_settings.c, included before xbox_SettingsInit (the table s_set and
 * xbox_SettingEnvString are above it). Kept in a file of its own. */

static void xs_vr_after_load(void)
{
    int i;

    /* In VR (the headset path set RECOMP_VR_ACTIVE) the VR page's picture quality replaces the Graphics page's: render scale,
     * anti-aliasing, sharpening, texture LOD bias. The Graphics rows are then taken as set from outside, so they keep what
     * the file says and are not saved over. A Graphics value the environment already set (a launcher) stays. */
    if (getenv("RECOMP_VR_ACTIVE")) {
        static const int k_vrmap[][2] = {
            { XS_VR_SCALE, XS_GRAPHICS_RENDER_SCALE }, { XS_VR_AA, XS_GRAPHICS_AA },
            { XS_VR_SHARPEN, XS_GRAPHICS_SHARPEN }, { XS_VR_LOD_BIAS, XS_GRAPHICS_LOD_BIAS },
        };
        for (i = 0; i < (int)(sizeof k_vrmap / sizeof k_vrmap[0]); i++) {
            xs_setting *v = &s_set[k_vrmap[i][0]], *g = &s_set[k_vrmap[i][1]];
            char buf[32];
            const char *e = v->locked ? getenv(recomp_env_name(v->env)) : NULL;

            if (g->locked)
                continue;
            if (e && *e)
                snprintf(buf, sizeof buf, "%s", e);
            else
                xbox_SettingEnvString(v, v->value, buf, sizeof buf);
#if defined(_WIN32)
            _putenv_s(recomp_env_name(g->env), buf);
#else
            setenv(recomp_env_name(g->env), buf, 1);
#endif
            g->locked = 1;
        }
    }
}

int xbox_SettingsPeek(const char *title, const char *key, char *out, unsigned n)
{
    char path[512], err[128];
    const char *over = getenv("RECOMP_SETTINGS"), *v;
    recomp_cfg *c;

    if (!out || !n)
        return 0;
    out[0] = 0;
    if (over && !strcmp(over, "none"))
        return 0;
    if (over && *over) {
        snprintf(path, sizeof path, "%s", over);
    } else {
        const char *dir = recomp_save_dir(title);

        if (!dir || !*dir)
            return 0;
        snprintf(path, sizeof path, "%s/settings.toml", dir);
    }
    c = recomp_cfg_load_file_flags(path, RCFG_REOPEN_TABLES, err, sizeof err);
    if (!c)
        return 0;
    v = recomp_cfg_string(c, key, NULL);
    if (v && *v)
        snprintf(out, n, "%s", v);
    recomp_cfg_free(c);
    return out[0] != 0;
}
