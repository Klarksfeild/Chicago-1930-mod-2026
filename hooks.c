/* Proxy SDL2.dll pour Chicago 1930 (Steam) - 32 bits.
 * Fait : résolution logique 1280x720, zoom molette centré sur le curseur,
 *        cadencement précis de la boucle de jeu. Les autres exports sont
 *        redirigés vers SDL2_real.dll (voir gen_def.py).
 * Aucune adresse mémoire du jeu n'est utilisée : tout passe par l'API SDL. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

typedef struct { int x, y, w, h; } Rect;

/* --- fonctions réelles --- */
static int   (*r_GetDesktopDisplayMode)(int, void *);
static void *(*r_CreateWindow)(const char *, int, int, int, int, uint32_t);
static void *(*r_CreateRenderer)(void *, int, uint32_t);
static int   (*r_RenderSetLogicalSize)(void *, int, int);
static void *(*r_CreateTexture)(void *, uint32_t, int, int, int);
static int   (*r_SetRenderTarget)(void *, void *);
static int   (*r_RenderCopy)(void *, void *, const Rect *, const Rect *);
static void  (*r_RenderPresent)(void *);
static int   (*r_PollEvent)(void *);
static void  (*r_Delay)(uint32_t);
static const char *(*r_GetHint)(const char *);
static int   (*r_SetHint)(const char *, const char *);

/* --- configuration (chicago_mod.ini) --- */
static int LW = 1280, LH = 720;
static int cfg_zoom = 1, cfg_fps = 100, cfg_vsync = 0, cfg_log = 1;
static float cfg_maxzoom = 3.0f, cfg_step = 1.15f;

/* --- état --- */
static void *g_ren, *g_tex;
static int g_inPresent;
static float z = 1.0f, vx = 0, vy = 0;   /* zoom, origine de la vue (px logiques) */
static int mx, my;                       /* curseur brut (coord. écran logiques) */
static FILE *g_log;

static void L(const char *f, ...) {
    if (!g_log) return;
    va_list a; va_start(a, f); vfprintf(g_log, f, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static void clampview(void) {
    float mxv = LW - LW / z, myv = LH - LH / z;
    if (vx < 0) vx = 0; if (vy < 0) vy = 0;
    if (vx > mxv) vx = mxv; if (vy > myv) vy = myv;
}

/* ===================== hooks ===================== */

int SDL_GetDesktopDisplayMode(int idx, void *mode) {
    int r = r_GetDesktopDisplayMode(idx, mode);
    if (r == 0 && mode) {
        int *m = (int *)mode;              /* format, w, h, refresh, driverdata */
        L("GetDesktopDisplayMode reel: %dx%d @%d", m[1], m[2], m[3]);
        m[1] = LW; m[2] = LH;              /* le jeu pense être en 1280x720 */
    }
    return r;
}

void *SDL_CreateWindow(const char *t, int x, int y, int w, int h, uint32_t fl) {
    L("CreateWindow %dx%d flags=0x%X", w, h, fl);
    if (fl & 0x1) fl = (fl & ~0x1u) | 0x1001;   /* FULLSCREEN -> FULLSCREEN_DESKTOP */
    return r_CreateWindow(t, x, y, w, h, fl);
}

void *SDL_CreateRenderer(void *win, int idx, uint32_t fl) {
    if (cfg_vsync) fl |= 0x4;                   /* PRESENTVSYNC */
    void *r = r_CreateRenderer(win, idx, fl);
    L("CreateRenderer -> %p flags=0x%X", r, fl);
    if (!r) return r;
    r_RenderSetLogicalSize(r, LW, LH);
    if (cfg_zoom && !g_ren) {
        char old[16] = "0";
        const char *o = r_GetHint("SDL_RENDER_SCALE_QUALITY");
        if (o) { strncpy(old, o, 15); old[15] = 0; }
        r_SetHint("SDL_RENDER_SCALE_QUALITY", "1");   /* linéaire pour la cible */
        g_tex = r_CreateTexture(r, 0x16362004 /*ARGB8888*/, 2 /*TARGET*/, LW, LH);
        r_SetHint("SDL_RENDER_SCALE_QUALITY", old);
        if (g_tex) { g_ren = r; r_SetRenderTarget(r, g_tex); }
        L("Cible hors-ecran: %p", g_tex);
    }
    return r;
}

int SDL_SetRenderTarget(void *r, void *t) {
    if (r == g_ren && g_tex && !t && !g_inPresent) t = g_tex;  /* garde notre cible */
    return r_SetRenderTarget(r, t);
}

void SDL_RenderPresent(void *r) {
    if (r == g_ren && g_tex) {
        g_inPresent = 1;
        r_SetRenderTarget(r, NULL);
        Rect s = { (int)vx, (int)vy, (int)(LW / z + 0.5f), (int)(LH / z + 0.5f) };
        r_RenderCopy(r, g_tex, &s, NULL);
        r_RenderPresent(r);
        r_SetRenderTarget(r, g_tex);
        g_inPresent = 0;
    } else r_RenderPresent(r);
}

int SDL_PollEvent(void *ev) {
    int ret = r_PollEvent(ev);
    if (!ret || !ev || !g_tex) return ret;
    uint32_t type = *(uint32_t *)ev;
    int32_t *e = (int32_t *)ev;
    if (type == 0x400) {                         /* MOUSEMOTION: x@20 y@24 xrel@28 yrel@32 */
        mx = e[5]; my = e[6];
        if (z > 1.0f) { e[5] = (int)(vx + mx / z); e[6] = (int)(vy + my / z);
                        e[7] = (int)(e[7] / z);   e[8] = (int)(e[8] / z); }
    } else if (type == 0x401 || type == 0x402) { /* BUTTON: x@20 y@24 */
        mx = e[5]; my = e[6];
        if (z > 1.0f) { e[5] = (int)(vx + mx / z); e[6] = (int)(vy + my / z); }
    } else if (type == 0x403) {                  /* MOUSEWHEEL: x@16 y@20 */
        int dy = e[5];
        if (dy) {
            float nz = z * powf(cfg_step, (float)dy);
            if (nz < 1.0f) nz = 1.0f;
            if (nz > cfg_maxzoom) nz = cfg_maxzoom;
            float gx = vx + mx / z, gy = vy + my / z;   /* point du monde sous le curseur */
            z = nz; vx = gx - mx / z; vy = gy - my / z;
            clampview();
            if (z <= 1.0f) { vx = vy = 0; }
        }
    }
    return ret;
}

void SDL_Delay(uint32_t ms) {
    static LARGE_INTEGER f, last;
    if (cfg_fps <= 0 || ms != 10) { r_Delay(ms); return; }   /* uniquement le delay de la boucle */
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LONGLONG per = f.QuadPart / cfg_fps;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG t = last.QuadPart + per;
    if (!last.QuadPart || now.QuadPart > t + 5 * per) t = now.QuadPart;   /* resynchro */
    while (now.QuadPart < t) {
        LONGLONG rest_ms = (t - now.QuadPart) * 1000 / f.QuadPart;
        if (rest_ms > 1) Sleep((DWORD)(rest_ms - 1)); else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    last.QuadPart = t;
}

/* ===================== init ===================== */

#define RES(var, name) do { *(FARPROC *)&var = GetProcAddress(h, name); \
    if (!var) { L("MANQUANT: %s", name); ok = 0; } } while (0)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID rsv) {
    if (why != DLL_PROCESS_ATTACH) return TRUE;
    char dir[MAX_PATH], p[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameA(inst, dir, MAX_PATH);
    char *s = strrchr(dir, '\\'); if (s) s[1] = 0;
    snprintf(ini, MAX_PATH, "%schicago_mod.ini", dir);
    cfg_log  = GetPrivateProfileIntA("mod", "log", 1, ini);
    cfg_zoom = GetPrivateProfileIntA("mod", "zoom", 1, ini);
    cfg_fps  = GetPrivateProfileIntA("mod", "fps", 100, ini);
    cfg_vsync= GetPrivateProfileIntA("mod", "vsync", 0, ini);
    LW = GetPrivateProfileIntA("mod", "width", 1280, ini);
    LH = GetPrivateProfileIntA("mod", "height", 720, ini);
    cfg_maxzoom = GetPrivateProfileIntA("mod", "maxzoom_x100", 300, ini) / 100.0f;
    if (cfg_log) { snprintf(p, MAX_PATH, "%schicago_mod.log", dir); g_log = fopen(p, "w"); }
    snprintf(p, MAX_PATH, "%sSDL2_real.dll", dir);
    HMODULE h = LoadLibraryA(p);
    if (!h) { L("SDL2_real.dll introuvable"); return FALSE; }
    int ok = 1;
    RES(r_GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode");
    RES(r_CreateWindow, "SDL_CreateWindow");
    RES(r_CreateRenderer, "SDL_CreateRenderer");
    RES(r_RenderSetLogicalSize, "SDL_RenderSetLogicalSize");
    RES(r_CreateTexture, "SDL_CreateTexture");
    RES(r_SetRenderTarget, "SDL_SetRenderTarget");
    RES(r_RenderCopy, "SDL_RenderCopy");
    RES(r_RenderPresent, "SDL_RenderPresent");
    RES(r_PollEvent, "SDL_PollEvent");
    RES(r_Delay, "SDL_Delay");
    RES(r_GetHint, "SDL_GetHint");
    RES(r_SetHint, "SDL_SetHint");
    timeBeginPeriod(1);
    L("Proxy charge. ok=%d logique=%dx%d zoom=%d fps=%d vsync=%d", ok, LW, LH, cfg_zoom, cfg_fps, cfg_vsync);
    return ok;
}
