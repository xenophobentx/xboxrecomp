/**
 * xbox_input touch -- the on-screen controls (touch_pad.h).
 *
 * Sizes are fractions of the screen's height times input.touch_scale, so a
 * control is the same share of the thumb's reach on a phone and on a 7"
 * handheld: GAS is 0.26 H across, about 17 mm on a 6" 1080p phone, the
 * small buttons 0.11 H (7 mm), all above Android's 48 dp touch target. On
 * a screen taller than 90 mm (a tablet) they keep the size they would have
 * at 90 mm (xbox_InputTouchSetDpi).
 * They keep 3.5% of the height from each edge, plus what a notch takes
 * (xbox_InputTouchSetInsets).
 *
 * The player's own places and sizes (the overlay's layout editor) go over
 * the designed ones: a centre as fractions of the screen, a size as a factor
 * of the designed one, per layout and control (g_ovr).
 */
#include "touch_pad.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&g_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* The controls; a finger holds one by this id, so a layout change lets go
 * of what the new layout does not have. */
enum ctl_id {
    C_NONE = -1,
    C_STEER, C_DPAD,
    C_GAS, C_BRAKE, C_BOOST, C_BREAKER, C_LOOK, C_VIEW,
    C_A, C_B, C_X, C_Y, C_LT, C_RT,
    C_PAUSE, C_MENU,
    C_COUNT
};

#define AN(i) (1u << (i))      /* an analog button's bit in .analog */

/* The controls' names in the layout file (xbox_InputTouchLayoutText). */
static const char *const k_name[C_COUNT] = {
    "steer", "dpad", "gas", "brake", "boost", "breaker", "look", "view",
    "a", "b", "x", "y", "lt", "rt", "pause", "menu"
};

typedef struct ctl {
    int id, kind, icon;
    int steer;                  /* DPAD: the left stick, eight ways (else the d-pad's buttons, four) */
    float x, y, r;
    WORD buttons;               /* wButtons bits */
    unsigned analog;            /* AN(XBOX_BUTTON_*) bits, held at 255 */
    const char *label;
    uint32_t rgb;
} ctl;

#define MAX_FINGERS 10
static struct finger {
    int used;
    int64_t id;
    int ctl;                    /* enum ctl_id; C_NONE: inert */
    int glide;                  /* went down on a button: may slide onto another */
    float x, y;                 /* now */
    unsigned read0;             /* g_reads when it went down */
    int lift;                   /* up, held on until the title has read the press twice */
} g_f[MAX_FINGERS];

static ctl   g_ctl[XBOX_TOUCH_MAX_CONTROLS];
static int   g_n;
static int   g_driving = -1;    /* the layout built: -1 none yet */
static float g_w = 1280.0f, g_h = 720.0f;
static float g_in[4];           /* left, top, right, bottom */
static float g_dpi;             /* 0: unknown */
static int   g_dirty = 1;       /* the screen or a setting changed: rebuild */

/* The player's changes: [driving][control]. */
#define LAY_SIZE_MIN 0.5f
#define LAY_SIZE_MAX 2.5f
static struct ovr {
    int on;
    float x, y;                 /* the centre, fractions of the screen */
    float k;                    /* the size, times the designed one */
} g_ovr[2][C_COUNT];

static volatile int   g_mode = XBOX_TOUCH_OFF;
static volatile float g_opacity = 0.5f, g_scale = 1.0f;
static volatile int   g_autogas;
static int (*g_driving_fn)(void);
static int g_hidden;            /* auto: a controller was used since the last touch */
static volatile int g_in_use;   /* the touch, not a controller, was the last used */
static volatile unsigned g_reads;   /* the title's reads of the state */
static volatile int g_menu_req;
static volatile int g_blocked;  /* the overlay is open */
static void (*g_activity)(void);

void xbox_InputTouchSetActivityHook(void (*fn)(void)) { g_activity = fn; }

/* Under g_lock: what the title reads. */
static XBOX_GAMEPAD g_out;
static DWORD g_pkt;

/* ── settings ─────────────────────────────────────────────────────── */

void xbox_InputTouchSetMode(int mode)
{
    if (mode < XBOX_TOUCH_AUTO || mode > XBOX_TOUCH_OFF)
        mode = XBOX_TOUCH_OFF;
    g_mode = mode;
}

int xbox_InputTouchMode(void) { return g_mode; }

void xbox_InputTouchSetOpacity(float o)
{
    g_opacity = o < 0.1f ? 0.1f : o > 1.0f ? 1.0f : o;
}

void xbox_InputTouchSetScale(float s)
{
    g_scale = s < 0.5f ? 0.5f : s > 2.0f ? 2.0f : s;
    g_dirty = 1;
}

void xbox_InputTouchSetAutoGas(int on) { g_autogas = on != 0; }

void xbox_InputTouchSetDrivingHook(int (*driving)(void)) { g_driving_fn = driving; }

void xbox_InputTouchSetScreen(float w, float h)
{
    if (w <= 0.0f || h <= 0.0f || (w == g_w && h == g_h))
        return;
    g_w = w;
    g_h = h;
    g_dirty = 1;
}

void xbox_InputTouchSetDpi(float dpi)
{
    dpi = dpi >= 100.0f && dpi <= 1200.0f ? dpi : 0.0f;
    if (dpi != g_dpi) {
        g_dpi = dpi;
        g_dirty = 1;
    }
}

void xbox_InputTouchSetInsets(float l, float t, float r, float b)
{
    float v[4] = { l, t, r, b };
    for (int i = 0; i < 4; i++) {
        v[i] = v[i] < 0.0f ? 0.0f : v[i];
        if (v[i] != g_in[i])
            g_dirty = 1;
        g_in[i] = v[i];
    }
}

void xbox_InputTouchReadInsets(void)
{
    const char *v = getenv("RECOMP_TOUCH_INSETS");
    float l = 0, t = 0, r = 0, b = 0;
    if (v && sscanf(v, "%f,%f,%f,%f", &l, &t, &r, &b) == 4)
        xbox_InputTouchSetInsets(l, t, r, b);
}

float xbox_InputTouchOpacity(void) { return g_opacity; }

int xbox_InputTouchVisible(void)
{
    return g_mode == XBOX_TOUCH_ALWAYS || (g_mode == XBOX_TOUCH_AUTO && !g_hidden);
}

/* ── the layouts ──────────────────────────────────────────────────── */

/* A layout being made (layout()): the controls go into out. */
typedef struct lay {
    ctl *out;
    int n;
} lay;

static ctl *add(lay *L, int id, int kind, float x, float y, float r, WORD buttons, unsigned analog,
                const char *label, uint32_t rgb, int icon)
{
    ctl *c;
    if (L->n >= XBOX_TOUCH_MAX_CONTROLS)
        return NULL;
    c = &L->out[L->n++];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->kind = kind;
    c->icon = icon;
    c->x = x;
    c->y = y;
    c->r = r;
    c->buttons = buttons;
    c->analog = analog;
    c->label = label;
    c->rgb = rgb;
    return c;
}

#define RGB_GREEN  0x5DBB2Eu
#define RGB_RED    0xE0402Fu
#define RGB_BLUE   0x3A7BE0u
#define RGB_YELLOW 0xF2C31Cu
#define RGB_ORANGE 0xFF8A1Cu

/* One layout as it stands now (the screen, the settings, the player's
 * changes) into out; the count. */
static int layout(int driving, ctl *out)
{
    /* The height the sizes are fractions of: the screen's, at most 90 mm. */
    const float cap = g_dpi > 0.0f ? 90.0f * g_dpi / 25.4f : g_h;
    const float base = g_h < cap ? g_h : cap;
    const float s = base * g_scale;
    const float m = 0.035f * base;
    const float xl = g_in[0] + m, yt = g_in[1] + m;
    const float xr = g_w - g_in[2] - m, yb = g_h - g_in[3] - m;
    lay L = { out, 0 };
    ctl *c;
#define R(v) ((v) * s)

    /* Clear of the HUD's corners in every picture setup: the title's POS,
     * LAP, speedometer and boost bar sit within 0.22 H of the top and the
     * bottom of the picture, its score board within 0.21 H either side of
     * the centre at the top (measured at 16:9 and 20:9, the HUD pinned to
     * the screen's edges, and 4:3 in the middle). V() is a fraction of the
     * screen's height (where the HUD is), R() a size (the thumb's). */
#define V(v) ((v) * g_h)
    /* Pause and the settings menu: top, left of the score board, right of
     * POS (and of where POS is in a 4:3 picture). */
    add(&L, C_PAUSE, XBOX_TOUCH_KIND_BUTTON, g_w * 0.5f - V(0.40f), yt + R(0.065f), R(0.055f),
        XBOX_GAMEPAD_START, 0, NULL, 0, XBOX_TOUCH_ICON_PAUSE);
    add(&L, C_MENU, XBOX_TOUCH_KIND_BUTTON, g_w * 0.5f - V(0.27f), yt + R(0.065f), R(0.055f),
        0, 0, NULL, 0, XBOX_TOUCH_ICON_MENU);

    if (driving) {
        /* The right cluster between LAP (top 0.19 H) and the speedometer
         * (bottom 0.15 H). */
        add(&L, C_GAS, XBOX_TOUCH_KIND_BUTTON, xr - R(0.15f), V(0.70f), R(0.12f),
            0, AN(XBOX_BUTTON_RTRIGGER), "GAS", RGB_GREEN, 0);
        add(&L, C_BRAKE, XBOX_TOUCH_KIND_BUTTON, xr - R(0.41f), V(0.74f), R(0.09f),
            0, AN(XBOX_BUTTON_LTRIGGER), "BRAKE", RGB_RED, 0);
        /* Boost is A with the accelerator still down: one thumb. */
        add(&L, C_BOOST, XBOX_TOUCH_KIND_BUTTON, xr - R(0.15f), V(0.45f), R(0.10f),
            0, AN(XBOX_BUTTON_A) | AN(XBOX_BUTTON_RTRIGGER), "BOOST", RGB_ORANGE, 0);
        add(&L, C_BREAKER, XBOX_TOUCH_KIND_BUTTON, xr - R(0.40f), V(0.52f), R(0.075f),
            0, AN(XBOX_BUTTON_B), "BREAKER", 0, 0);
        add(&L, C_LOOK, XBOX_TOUCH_KIND_BUTTON, xr - R(0.38f), V(0.31f), R(0.065f),
            0, AN(XBOX_BUTTON_X), "LOOK", 0, 0);
        add(&L, C_VIEW, XBOX_TOUCH_KIND_BUTTON, xr - R(0.15f), V(0.26f), R(0.06f),
            0, AN(XBOX_BUTTON_Y), "VIEW", 0, 0);
        /* Steering: a d-pad that stays where it is drawn, above the boost
         * bar. Its centre never moves, so a thumb that lands right of it
         * steers right, and slid across steers left at once. */
        c = add(&L, C_STEER, XBOX_TOUCH_KIND_DPAD, xl + R(0.25f), V(0.64f), R(0.19f),
                0, 0, NULL, 0, 0);
        if (c)
            c->steer = 1;
    } else {
        add(&L, C_DPAD, XBOX_TOUCH_KIND_DPAD, xl + R(0.25f), yb - R(0.25f), R(0.22f),
            0, 0, NULL, 0, 0);
        add(&L, C_A, XBOX_TOUCH_KIND_BUTTON, xr - R(0.22f), yb - R(0.10f), R(0.08f),
            0, AN(XBOX_BUTTON_A), "A", RGB_GREEN, 0);
        add(&L, C_B, XBOX_TOUCH_KIND_BUTTON, xr - R(0.08f), yb - R(0.24f), R(0.08f),
            0, AN(XBOX_BUTTON_B), "B", RGB_RED, 0);
        add(&L, C_X, XBOX_TOUCH_KIND_BUTTON, xr - R(0.36f), yb - R(0.24f), R(0.08f),
            0, AN(XBOX_BUTTON_X), "X", RGB_BLUE, 0);
        add(&L, C_Y, XBOX_TOUCH_KIND_BUTTON, xr - R(0.22f), yb - R(0.38f), R(0.08f),
            0, AN(XBOX_BUTTON_Y), "Y", RGB_YELLOW, 0);
        add(&L, C_LT, XBOX_TOUCH_KIND_BUTTON, xl + R(0.10f), yb - R(0.62f), R(0.07f),
            0, AN(XBOX_BUTTON_LTRIGGER), "LT", 0, 0);
        add(&L, C_RT, XBOX_TOUCH_KIND_BUTTON, xr - R(0.10f), yb - R(0.62f), R(0.07f),
            0, AN(XBOX_BUTTON_RTRIGGER), "RT", 0, 0);
    }
#undef V
#undef R

    /* The player's places and sizes, each control kept whole on the screen. */
    for (int i = 0; i < L.n; i++) {
        const struct ovr *o = &g_ovr[driving != 0][out[i].id];
        c = &out[i];
        if (o->on) {
            c->x = o->x * g_w;
            c->y = o->y * g_h;
            c->r *= o->k;
        }
        if (2.0f * c->r < g_w)
            c->x = c->x < c->r ? c->r : c->x > g_w - c->r ? g_w - c->r : c->x;
        if (2.0f * c->r < g_h)
            c->y = c->y < c->r ? c->r : c->y > g_h - c->r ? g_h - c->r : c->y;
    }
    return L.n;
}

static void build(int driving)
{
    g_n = layout(driving, g_ctl);
}

static const ctl *find(int id)
{
    for (int i = 0; i < g_n; i++)
        if (g_ctl[i].id == id)
            return &g_ctl[i];
    return NULL;
}

/* The layout for now: the title's mode, the screen, the settings. */
static void refresh(void)
{
    int d = g_driving_fn ? g_driving_fn() != 0 : 0;

    if (d == g_driving && !g_dirty)
        return;
    g_driving = d;
    g_dirty = 0;
    build(d);
    /* A finger on a control the new layout has not is let go of until it
     * lifts (the GAS thumb as a race ends does not become a menu's A). */
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].ctl != C_NONE && !find(g_f[i].ctl)) {
            g_f[i].ctl = C_NONE;
            g_f[i].glide = 0;
        }
}

/* ── the pad they make ────────────────────────────────────────────── */

static float dist(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

/* The d-pad's direction from the centre, nothing in the middle fifth. The
 * menus' takes the dominant axis only (a menu wants no diagonals); the
 * steering one has eight ways: an axis counts within 67.5 degrees of it, so
 * a thumb a little off left still steers fully left. *ux, *uy: -1 0 1. */
static WORD dpad_dir(const ctl *c, float x, float y, float *ux, float *uy)
{
    float dx = x - c->x, dy = y - c->y;
    *ux = *uy = 0.0f;
    if (dist(x, y, c->x, c->y) < c->r * 0.2f)
        return 0;
    if (c->steer) {
        if (fabsf(dy) < fabsf(dx) * 2.4142f)
            *ux = dx < 0 ? -1.0f : 1.0f;
        if (fabsf(dx) < fabsf(dy) * 2.4142f)
            *uy = dy < 0 ? -1.0f : 1.0f;
        return 0;
    }
    if (fabsf(dx) >= fabsf(dy)) {
        *ux = dx < 0 ? -1.0f : 1.0f;
        return dx < 0 ? XBOX_GAMEPAD_DPAD_LEFT : XBOX_GAMEPAD_DPAD_RIGHT;
    }
    *uy = dy < 0 ? -1.0f : 1.0f;
    return dy < 0 ? XBOX_GAMEPAD_DPAD_UP : XBOX_GAMEPAD_DPAD_DOWN;
}

static SHORT axis(float v)
{
    v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return (SHORT)lrintf(v * 32767.0f);
}

static void publish(void)
{
    XBOX_GAMEPAD g;
    int brake = 0;

    memset(&g, 0, sizeof g);
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].lift && g_reads - g_f[i].read0 >= 2)
            g_f[i].used = 0;
    if (xbox_InputTouchVisible()) {
        for (int i = 0; i < MAX_FINGERS; i++) {
            const struct finger *f = &g_f[i];
            const ctl *c;
            if (!f->used || f->ctl == C_NONE || !(c = find(f->ctl)))
                continue;
            if (c->kind == XBOX_TOUCH_KIND_DPAD) {
                float ux, uy;
                g.wButtons |= dpad_dir(c, f->x, f->y, &ux, &uy);
                if (c->steer) {
                    g.sThumbLX = axis(ux);
                    g.sThumbLY = axis(-uy);    /* up is positive on the pad */
                }
            } else {
                g.wButtons |= c->buttons;
                for (int b = 0; b < 8; b++)
                    if (c->analog & AN(b))
                        g.bAnalogButtons[b] = 255;
                if (c->id == C_BRAKE)
                    brake = 1;
            }
        }
        if (g_autogas && g_driving == 1 && !brake)
            g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
    }
    LOCK();
    if (memcmp(&g, &g_out, sizeof g)) {
        g_out = g;
        g_pkt++;
    }
    UNLOCK();
}

int xbox_InputTouchState(XBOX_INPUT_STATE *out)
{
    if (g_mode == XBOX_TOUCH_OFF)
        return 0;
    LOCK();
    out->Gamepad = g_out;
    out->dwPacketNumber = g_pkt;
    g_reads++;
    UNLOCK();
    return 1;
}

int xbox_InputTouchInUse(void)
{
    return g_mode != XBOX_TOUCH_OFF && g_in_use;
}

int xbox_InputTouchTakeMenu(void)
{
    int r = g_menu_req;
    g_menu_req = 0;
    return r;
}

/* ── fingers ──────────────────────────────────────────────────────── */

/* The control under a point: the nearest whose circle, made `slack` times
 * larger, holds it. */
static const ctl *hit(float x, float y, float slack, int buttons_only)
{
    const ctl *best = NULL;
    float bd = 0.0f;
    for (int i = 0; i < g_n; i++) {
        const ctl *c = &g_ctl[i];
        float d;
        if (buttons_only && c->kind != XBOX_TOUCH_KIND_BUTTON)
            continue;
        d = dist(x, y, c->x, c->y);
        if (d <= c->r * slack && (!best || d / c->r < bd)) {
            best = c;
            bd = d / c->r;
        }
    }
    return best;
}

static struct finger *finger_of(int64_t id)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].id == id)
            return &g_f[i];
    return NULL;
}

static void press(const ctl *c)
{
    if (c && c->id == C_MENU)
        g_menu_req = 1;
}

void xbox_InputTouchFinger(int what, int64_t id, float x, float y)
{
    struct finger *f;

    if (g_mode == XBOX_TOUCH_OFF || g_blocked)
        return;
    refresh();
    f = finger_of(id);
    if (what == XBOX_TOUCH_DOWN) {
        const ctl *c;
        if (g_mode == XBOX_TOUCH_AUTO)
            g_hidden = 0;               /* a touch brings them back */
        g_in_use = 1;
        if (!f) {
            for (int i = 0; i < MAX_FINGERS && !f; i++)
                if (!g_f[i].used)
                    f = &g_f[i];
            if (!f)
                return;                 /* an eleventh finger */
        }
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->id = id;
        f->x = x;
        f->y = y;
        f->ctl = C_NONE;
        f->read0 = g_reads;
        c = hit(x, y, 1.25f, 0);
        if (c && g_activity)
            g_activity();
        if (c) {
            f->ctl = c->id;
            f->glide = c->kind == XBOX_TOUCH_KIND_BUTTON;
            press(c);
        }
    } else if (!f) {
        return;
    } else if (what == XBOX_TOUCH_MOVE) {
        f->x = x;
        f->y = y;
        if (f->glide) {
            /* Onto another button: that one at once (GAS to BOOST and back
             * without lifting). Off every button: the one held stays held
             * until the finger is well away from it, so a thumb rolling
             * between two buttons never drops the throttle for a frame. */
            const ctl *c = f->ctl != C_NONE ? find(f->ctl) : NULL;
            const ctl *n = hit(x, y, 1.1f, 1);
            if (n && n != c) {
                f->ctl = n->id;
                f->read0 = g_reads;
                press(n);
            } else if (c && !n && dist(x, y, c->x, c->y) > c->r * 1.6f) {
                f->ctl = C_NONE;
            }
        }
    } else {
        /* A tap shorter than two of the title's reads would be missed (it
         * polls once a frame): the press is held until it has been read. */
        const ctl *c = f->ctl != C_NONE ? find(f->ctl) : NULL;
        if (c && g_reads - f->read0 < 2)
            f->lift = 1;
        else
            f->used = 0;
    }
    publish();
}

void xbox_InputTouchReleaseAll(void)
{
    memset(g_f, 0, sizeof g_f);
    publish();
}

void xbox_InputTouchSetBlocked(int on)
{
    g_blocked = on != 0;
    if (g_blocked)
        xbox_InputTouchReleaseAll();
}

void xbox_InputTouchControllerUsed(void)
{
    g_in_use = 0;
    if (g_mode != XBOX_TOUCH_AUTO || g_hidden)
        return;
    g_hidden = 1;
    xbox_InputTouchReleaseAll();
}

/* ── drawing ──────────────────────────────────────────────────────── */

int xbox_InputTouchDrawList(xbox_touch_draw *out, int max)
{
    int n = 0;

    if (!xbox_InputTouchVisible())
        return 0;
    refresh();
    publish();                          /* the mode may have changed under a held GAS */
    for (int i = 0; i < g_n && n < max; i++) {
        const ctl *c = &g_ctl[i];
        xbox_touch_draw *d = &out[n++];
        memset(d, 0, sizeof *d);
        d->id = c->id;
        d->kind = c->kind;
        d->icon = c->icon;
        d->x = c->x;
        d->y = c->y;
        d->r = c->r;
        d->label = c->label;
        d->rgb = c->rgb;
        for (int k = 0; k < MAX_FINGERS; k++) {
            const struct finger *f = &g_f[k];
            if (!f->used || f->ctl != c->id)
                continue;
            d->pressed = 1;
            if (c->kind == XBOX_TOUCH_KIND_DPAD) {
                float ux, uy;
                dpad_dir(c, f->x, f->y, &ux, &uy);
                if (ux == 0.0f && uy == 0.0f)
                    d->pressed = 0;
                d->kx = ux;
                d->ky = uy;
            }
        }
        if (c->kind == XBOX_TOUCH_KIND_DPAD && !d->pressed)
            d->kx = d->ky = 0.0f;
    }
    return n;
}

/* ── the layout editor ────────────────────────────────────────────── */

static int lay_index(int driving) { return driving != 0; }

int xbox_InputTouchLayoutList(int driving, xbox_touch_draw *out, int max)
{
    ctl c[XBOX_TOUCH_MAX_CONTROLS];
    int n = layout(lay_index(driving), c), k = 0;

    for (int i = 0; i < n && k < max; i++, k++) {
        xbox_touch_draw *d = &out[k];
        memset(d, 0, sizeof *d);
        d->id = c[i].id;
        d->kind = c[i].kind;
        d->icon = c[i].icon;
        d->x = c[i].x;
        d->y = c[i].y;
        d->r = c[i].r;
        d->label = c[i].label;
        d->rgb = c[i].rgb;
    }
    return k;
}

float xbox_InputTouchLayoutSize(int driving, int id)
{
    const struct ovr *o;
    if (id < 0 || id >= C_COUNT)
        return 1.0f;
    o = &g_ovr[lay_index(driving)][id];
    return o->on ? o->k : 1.0f;
}

void xbox_InputTouchLayoutMove(int driving, int id, float x, float y, float size)
{
    struct ovr *o;
    if (id < 0 || id >= C_COUNT || g_w <= 0.0f || g_h <= 0.0f)
        return;
    o = &g_ovr[lay_index(driving)][id];
    x /= g_w;
    y /= g_h;
    o->on = 1;
    o->x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    o->y = y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
    o->k = size < LAY_SIZE_MIN ? LAY_SIZE_MIN : size > LAY_SIZE_MAX ? LAY_SIZE_MAX : size;
    g_dirty = 1;
}

void xbox_InputTouchLayoutReset(int driving)
{
    if (driving < 0)
        memset(g_ovr, 0, sizeof g_ovr);
    else
        memset(g_ovr[lay_index(driving)], 0, sizeof g_ovr[0]);
    g_dirty = 1;
}

int xbox_InputTouchLayoutText(char *out, int n)
{
    int len = 0;
#define PUT(...) do { int w_ = snprintf(out && len < n ? out + len : NULL, \
                                        out && len < n ? (size_t)(n - len) : 0, __VA_ARGS__); \
                      if (w_ > 0) len += w_; } while (0)
    if (out && n > 0)
        out[0] = 0;
    PUT("# Takedawn touch controls: <race|menus> <control> <x> <y> <size>\n"
        "# x and y: the centre, fractions of the screen; size: times the designed one\n");
    for (int d = 1; d >= 0; d--)
        for (int i = 0; i < C_COUNT; i++) {
            const struct ovr *o = &g_ovr[d][i];
            if (o->on)
                PUT("%s %s %.4f %.4f %.2f\n", d ? "race" : "menus", k_name[i], o->x, o->y, o->k);
        }
#undef PUT
    return len;
}

void xbox_InputTouchLayoutParse(const char *text)
{
    memset(g_ovr, 0, sizeof g_ovr);
    g_dirty = 1;
    while (text && *text) {
        char which[16], name[16];
        float x, y, k;
        const char *nl = strchr(text, '\n');
        if (*text != '#' && sscanf(text, "%15s %15s %f %f %f", which, name, &x, &y, &k) == 5) {
            int d = !strcmp(which, "race") ? 1 : !strcmp(which, "menus") ? 0 : -1;
            for (int i = 0; d >= 0 && i < C_COUNT; i++)
                if (!strcmp(name, k_name[i]) && x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f) {
                    g_ovr[d][i].on = 1;
                    g_ovr[d][i].x = x;
                    g_ovr[d][i].y = y;
                    g_ovr[d][i].k = k < LAY_SIZE_MIN ? LAY_SIZE_MIN : k > LAY_SIZE_MAX ? LAY_SIZE_MAX : k;
                }
        }
        text = nl ? nl + 1 : NULL;
    }
}

int xbox_InputTouchLayoutLoad(const char *path)
{
    char buf[4096];
    size_t got;
    FILE *f = path && *path ? fopen(path, "rb") : NULL;

    if (!f) {
        xbox_InputTouchLayoutReset(-1);
        return 0;
    }
    got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    xbox_InputTouchLayoutParse(buf);
    return 1;
}

int xbox_InputTouchLayoutSave(const char *path)
{
    char buf[4096], tmp[1100];
    int len = xbox_InputTouchLayoutText(buf, sizeof buf);
    FILE *f;

    if (!path || !*path || len >= (int)sizeof buf
            || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return 0;
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    if (fwrite(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        remove(tmp);
        return 0;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
    remove(path);                       /* Windows' rename does not replace */
    return rename(tmp, path) == 0;
}

void xbox_InputTouchReset(void)
{
    memset(g_ovr, 0, sizeof g_ovr);
    memset(g_f, 0, sizeof g_f);
    g_n = 0;
    g_driving = -1;
    g_dirty = 1;
    g_w = 1280.0f;
    g_h = 720.0f;
    memset(g_in, 0, sizeof g_in);
    g_dpi = 0.0f;
    g_mode = XBOX_TOUCH_OFF;
    g_opacity = 0.5f;
    g_scale = 1.0f;
    g_autogas = 0;
    g_driving_fn = NULL;
    g_hidden = 0;
    g_in_use = 0;
    g_reads = 0;
    g_menu_req = 0;
    g_blocked = 0;
    LOCK();
    memset(&g_out, 0, sizeof g_out);
    g_pkt = 0;
    UNLOCK();
}
