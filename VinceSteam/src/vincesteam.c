// VinceSteam: brings back the heat shimmer ("steam") over the Propeller Room vents and every other
// HeatShimmer in Voodoo Vince Remastered (Steam 1.14.2.0). Loaded by Vince.exe through an added import
// (tools/patch_import.py); everything else happens in memory at start-up.
//
// Why it was missing: the remaster's HeatShimmer (vtable 0xAD9440) has no Render, slot 7 is the shared
// `ret 4` at 0x407F70, and its constructor (0x45E230) never sets the node bounds. The bounds stay empty
// (FLT_MAX / -FLT_MAX from the base constructor), so the scene collector (0x4A79C0, node types 6 and 10)
// culls the node and it never reaches the alpha list. Its DX11 shaders are in the exe but nothing uses them.
//
// How it works:
//   - DllMain checks every game function and site it uses against the bytes of the base exe
//     (src/sites.h) plus the HeatShimmer vtable. On any mismatch it logs the failure and does nothing else.
//   - Constructor tail (0x45E27F): calls the game's own SetEllipsoidRadius (0x45E2F0) with the constructor's
//     radius, which sets the bounds to +-radius, as the Xbox constructor does (prototype 0xCC4FF).
//   - Vtable slot 7 -> hs_render(). The engine now culls, sorts and calls it in the alpha pass with the
//     node's world matrix, and skips hidden nodes (the vent switches hide them with SetOption 2).
//   - hs_render() copies the bound render target and draws a camera-facing quad that shows the copy through
//     two scrolling noise lookups and a circle mask: the Xbox HeatShimmer::Render (prototype 0xCBE30) with
//     its parameters, defaults and size. Every D3D11 state it touches is saved first and restored after,
//     so the game's own state cache stays right.

typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
typedef int BOOL; typedef void *HANDLE; typedef void *HMODULE; typedef long HRESULT;
typedef struct { long long q; } LARGE_INTEGER;
#define WINAPI __stdcall

__declspec(dllimport) BOOL WINAPI VirtualProtect(void *, u32, u32, u32 *);
__declspec(dllimport) BOOL WINAPI FlushInstructionCache(HANDLE, const void *, u32);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
typedef unsigned short WCHAR;
__declspec(dllimport) u32 WINAPI GetModuleFileNameW(HMODULE, WCHAR *, u32);
__declspec(dllimport) HANDLE WINAPI CreateFileW(const WCHAR *, u32, u32, void *, u32, u32, HANDLE);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void *, u32, u32 *, void *);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(const char *);
__declspec(dllimport) u32 WINAPI GetPrivateProfileIntW(const WCHAR *, const WCHAR *, int, const WCHAR *);
__declspec(dllimport) BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *);
__declspec(dllimport) BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *);

#ifndef VINCE_AIO /* AIO core (aio/core, 1.4 T6): _fltused and memset come from VinceAudio's bridge_engine.c */
int _fltused = 1;
void *memset(void *d, int c, unsigned n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
#else
void *memset(void *d, int c, unsigned n);
#endif
void *memcpy(void *d, const void *s, unsigned n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

/* ---- log: VinceSteam.log next to Vince.exe (wide path: any folder name works; empty = no log) ---- */
static WCHAR g_logpath[520 + 16];
static void logs(const char *s) {
    if (!g_logpath[0]) return;
    HANDLE h = CreateFileW(g_logpath, 4 /*FILE_APPEND_DATA*/, 1, 0, 4 /*OPEN_ALWAYS*/, 0x80, 0);
    if (h == (HANDLE)-1) return;
    u32 n = 0, w; while (s[n]) n++;
    WriteFile(h, s, n, &w, 0); WriteFile(h, "\r\n", 2, &w, 0); CloseHandle(h);
}
static void hex8(char *o, u32 v) { for (int i = 7; i >= 0; i--, v >>= 4) o[i] = "0123456789ABCDEF"[v & 15]; }
static void logx(const char *s, u32 v) {
    char m[120]; u32 n = 0; while (s[n] && n < 100) { m[n] = s[n]; n++; }
    m[n++] = ' '; m[n++] = '0'; m[n++] = 'x'; hex8(m + n, v); m[n + 8] = 0; logs(m);
}

/* ---- game ---- */
static u32 D; /* ASLR delta: actual base - 0x400000 */
#define R(va) ((u32)(va) + D)
#ifndef G_DEV                         /* tests/d3d_test.c supplies its own */
#define G_DEV (*(u8 **)R(0xB65538))   /* D3D11 renderer / state cache */
#endif
#define DEV_CTX   0x0C                /* ID3D11DeviceContext* */
#define DEV_VIEW  0x54                /* float[16] view, row vectors (p * V) */
#define DEV_PROJ  0x94                /* float[16] projection */
#define DEV_ZWRITE 0x7A0              /* cached render state 0xE (ZWRITEENABLE), dev+0x768+state*4 (0x45FDD0) */

#define HS_VTABLE   0xAD9440
#define HS_DTOR     0x45E290          /* slot 0 */
#define HS_BOUNDS   0x42DB00          /* slot 1, SwampSceneNode::GetBounds */
#define HS_NORENDER 0x407F70          /* slot 7 before the fix: ret 4 */
#define HS_RADIUS   0xA8              /* float[3]; the Xbox used x */
#define HS_SCALE    0xB4              /* float[2][2] noise uv scale per pass (Xbox +0xB0) */
#define HS_RATE     0xC4              /* float[2][2] noise uv offset rate per pass (Xbox +0xC0) */
#define HS_PERTURB  0xD4              /* float[2][2] perturb range per pass (Xbox +0xD0) */

typedef void(__thiscall *SetRadius_t)(u8 *node, const float *radius);   /* ret 4 */
#define SetEllipsoidRadius ((SetRadius_t)R(0x45E2F0))
typedef void(__thiscall *SetOption_t)(u8 *node, u32 opt, int on, int recurse);   /* ret 0xC */
#define SetOption ((SetOption_t)R(0x429030))
#define OPT_NO_OCCLUSION 0x20000      /* 0x4A5F5D: nodes with it skip the occlusion query (the game sets it on big ones) */

/* ---- D3D11, the few pieces used here ---- */
#define VT(o) (*(void ***)(o))
#define CALL(o, i, T) ((T)VT(o)[i])
static void rel(void *o) { if (o) ((u32(WINAPI *)(void *))VT(o)[2])(o); }

typedef struct { u32 Width, Height, MipLevels, ArraySize, Format, Count, Quality, Usage, BindFlags, CPUAccessFlags, MiscFlags; } TexDesc;
typedef struct { const void *mem; u32 pitch, slice; } SubData;
typedef struct { u32 Format, ViewDimension, a, b, c; } ViewDesc;   /* RTV / SRV: union starts at a */
enum { CTX_GetDevice = 3, VSSetConstantBuffers = 7, PSSetShaderResources = 8, PSSetShader = 9, PSSetSamplers = 10,
       VSSetShader = 11, Draw = 13, PSSetConstantBuffers = 16, IASetInputLayout = 17, GSSetShader = 23,
       IASetPrimitiveTopology = 24, OMSetBlendState = 35, OMSetDepthStencilState = 36, RSSetState = 43, RSGetViewports = 95,
       CopySubresourceRegion = 46, UpdateSubresource = 48, ResolveSubresource = 57,
       VSGetConstantBuffers = 72, PSGetShaderResources = 73, PSGetShader = 74, PSGetSamplers = 75, VSGetShader = 76,
       PSGetConstantBuffers = 77, IAGetInputLayout = 78, GSGetShader = 82, IAGetPrimitiveTopology = 83,
       OMGetRenderTargets = 89, OMGetBlendState = 91, OMGetDepthStencilState = 92, RSGetState = 94 };
enum { CreateBuffer = 3, CreateTexture2D = 5, CreateShaderResourceView = 7, CreateVertexShader = 12,
       CreatePixelShader = 15, CreateBlendState = 20, CreateDepthStencilState = 21, CreateRasterizerState = 22,
       CreateSamplerState = 23 };
enum { View_GetResource = 7, View_GetDesc = 8, Res_GetType = 7, Tex2D_GetDesc = 10 };

typedef void(WINAPI *SetN_t)(void *, u32, u32, void *const *);
typedef void(WINAPI *GetN_t)(void *, u32, u32, void **);
typedef void(WINAPI *SetSh_t)(void *, void *, void *const *, u32);
typedef void(WINAPI *GetSh_t)(void *, void **, void **, u32 *);
typedef void(WINAPI *Set1_t)(void *, void *);
typedef void(WINAPI *Get1_t)(void *, void **);
typedef HRESULT(WINAPI *Create_t)(void *, const void *, const void *, void **);
typedef HRESULT(WINAPI *CreateSh_t)(void *, const void *, u32, void *, void **);
typedef HRESULT(WINAPI *CreateState_t)(void *, const void *, void **);

#include "shaders.h"

typedef struct {
    float corner[4][4];
    float noise0[4], noise1[4], perturb[4], screen[4];
} Consts;

static struct {
    void *dev, *vs, *ps, *cb, *noise, *noiseSRV, *wrap, *clamp, *blend, *depthLE, *depthGE, *raster;
    void *copy, *copySRV; u32 cw, ch, cfmt, cvfmt;
    int failed;
} G;

static int g_enabled = 1, g_debug = 0;
static float g_strength = 1.0f;
static double g_qpcScale; static long long g_qpc0;
static u32 g_drawn, g_skipped;

static void release_all(void) {
    void **p[] = { &G.vs, &G.ps, &G.cb, &G.noise, &G.noiseSRV, &G.wrap, &G.clamp, &G.blend, &G.depthLE,
                   &G.depthGE, &G.raster, &G.copy, &G.copySRV };
    for (u32 i = 0; i < sizeof p / sizeof p[0]; i++) { rel(*p[i]); *p[i] = 0; }
    G.cw = G.ch = G.cfmt = G.cvfmt = 0; G.dev = 0;
}

static int create_all(void *dev) {
    HRESULT hr = 0;
    hr |= CALL(dev, CreateVertexShader, CreateSh_t)(dev, g_vs, sizeof g_vs, 0, &G.vs);
    hr |= CALL(dev, CreatePixelShader, CreateSh_t)(dev, g_ps, sizeof g_ps, 0, &G.ps);
    u32 bd[6] = { sizeof(Consts), 0 /*DEFAULT*/, 4 /*CONSTANT_BUFFER*/, 0, 0, 0 };
    hr |= CALL(dev, CreateBuffer, Create_t)(dev, bd, 0, &G.cb);

    /* the Xbox noise: random bytes (BeepLib::Random) in an 8x8x8 L8 volume; one 8x8 slice here */
    u8 px[64]; u32 seed = 0x5EEDu;
    for (u32 i = 0; i < 64; i++) { seed = seed * 1103515245u + 12345u; px[i] = (u8)(seed >> 16); }
    TexDesc td = { 8, 8, 1, 1, 61 /*R8_UNORM*/, 1, 0, 1 /*IMMUTABLE*/, 8 /*SHADER_RESOURCE*/, 0, 0 };
    SubData sd = { px, 8, 0 };
    hr |= CALL(dev, CreateTexture2D, Create_t)(dev, &td, &sd, &G.noise);
    if (G.noise) hr |= CALL(dev, CreateShaderResourceView, Create_t)(dev, G.noise, 0, &G.noiseSRV);

    u32 smp[13] = { 0x15 /*MIN_MAG_MIP_LINEAR*/, 1, 1, 1, 0, 1, 1 /*NEVER*/, 0, 0, 0, 0, 0, 0x7F7FFFFF };
    hr |= CALL(dev, CreateSamplerState, CreateState_t)(dev, smp, &G.wrap);
    smp[1] = smp[2] = smp[3] = 3; /* CLAMP */
    hr |= CALL(dev, CreateSamplerState, CreateState_t)(dev, smp, &G.clamp);

    /* SRCALPHA / INVSRCALPHA as on the Xbox; colour only, the target's alpha is left alone */
    u32 bl[2 + 8 * 8]; memset(bl, 0, sizeof bl);
    u32 rt0[8] = { 1, 5 /*SRC_ALPHA*/, 6 /*INV_SRC_ALPHA*/, 1 /*ADD*/, 1 /*ZERO*/, 2 /*ONE*/, 1, 7 /*RGB*/ };
    memcpy(bl + 2, rt0, sizeof rt0);
    hr |= CALL(dev, CreateBlendState, CreateState_t)(dev, bl, &G.blend);

    /* depth test, no depth writes (Xbox ZWRITEENABLE off); the test direction is picked per frame */
    u32 ds[13] = { 1, 0 /*WRITE_ZERO*/, 4 /*LESS_EQUAL*/, 0, 0, 1, 1, 1, 8, 1, 1, 1, 8 };
    hr |= CALL(dev, CreateDepthStencilState, CreateState_t)(dev, ds, &G.depthLE);
    ds[2] = 7; /* GREATER_EQUAL */
    hr |= CALL(dev, CreateDepthStencilState, CreateState_t)(dev, ds, &G.depthGE);

    u32 rs[10] = { 3 /*SOLID*/, 1 /*CULL_NONE*/, 0, 0, 0, 0, 1 /*DepthClip*/, 0, 0, 0 };
    hr |= CALL(dev, CreateRasterizerState, CreateState_t)(dev, rs, &G.raster);

    if (hr < 0 || !G.vs || !G.ps || !G.cb || !G.noiseSRV || !G.wrap || !G.clamp || !G.blend || !G.depthLE ||
        !G.depthGE || !G.raster) {
        logx("shimmer: creating the D3D11 objects failed, hr", (u32)hr);
        return 0;
    }
    G.dev = dev;
    logs("shimmer: D3D11 objects ready");
    return 1;
}

/* the scene copy, made again when the target's size or format changes */
static int ensure_copy(u32 w, u32 h, u32 fmt, u32 vfmt) {
    if (G.copy && G.cw == w && G.ch == h && G.cfmt == fmt && G.cvfmt == vfmt) return 1;
    rel(G.copySRV); rel(G.copy); G.copy = G.copySRV = 0;
    TexDesc td = { w, h, 1, 1, fmt, 1, 0, 0 /*DEFAULT*/, 8 /*SHADER_RESOURCE*/, 0, 0 };
    if (CALL(G.dev, CreateTexture2D, Create_t)(G.dev, &td, 0, &G.copy) < 0 || !G.copy) {
        logx("shimmer: cannot copy a target of format", fmt); return 0;
    }
    ViewDesc vd = { vfmt, 4 /*TEXTURE2D*/, 0, 1, 0 };
    if (CALL(G.dev, CreateShaderResourceView, Create_t)(G.dev, G.copy, &vd, &G.copySRV) < 0 || !G.copySRV) {
        logx("shimmer: cannot view a copy of format", vfmt); rel(G.copy); G.copy = 0; return 0;
    }
    G.cw = w; G.ch = h; G.cfmt = fmt; G.cvfmt = vfmt;
    return 1;
}

/* depth target size (0 x 0 when none is bound), for the diagnostics */
static u32 g_dsW, g_dsH;
static void depth_size(void *dsv) {
    void *res = 0; u32 type = 0; TexDesc td;
    g_dsW = g_dsH = 0;
    if (!dsv) return;
    CALL(dsv, View_GetResource, Get1_t)(dsv, &res);
    if (!res) return;
    ((void(WINAPI *)(void *, u32 *))VT(res)[Res_GetType])(res, &type);
    if (type == 3) { CALL(res, Tex2D_GetDesc, Set1_t)(res, &td); g_dsW = td.Width; g_dsH = td.Height; }
    rel(res);
}

/* Copies the bound render target into G.copy. Returns its size in *w, *h. */
static int copy_target(void *ctx, u32 *w, u32 *h) {
    void *rtv = 0, *dsv = 0, *res = 0; int ok = 0;
    ((void(WINAPI *)(void *, u32, void **, void **))VT(ctx)[OMGetRenderTargets])(ctx, 1, &rtv, &dsv);
    depth_size(dsv);
    rel(dsv);
    if (!rtv) return 0;
    CALL(rtv, View_GetResource, Get1_t)(rtv, &res);
    ViewDesc vd; CALL(rtv, View_GetDesc, Set1_t)(rtv, &vd);
    if (res) {
        u32 type = 0; ((void(WINAPI *)(void *, u32 *))VT(res)[Res_GetType])(res, &type);
        TexDesc td; if (type == 3) CALL(res, Tex2D_GetDesc, Set1_t)(res, &td);
        if (type == 3 && (vd.ViewDimension == 4 || vd.ViewDimension == 6)) {
            u32 mip = vd.ViewDimension == 4 ? vd.a : 0;
            u32 cw = td.Width >> mip, ch = td.Height >> mip;
            if (cw && ch && ensure_copy(cw, ch, td.Format, vd.Format)) {
                if (vd.ViewDimension == 6)
                    ((void(WINAPI *)(void *, void *, u32, void *, u32, u32))VT(ctx)[ResolveSubresource])(ctx, G.copy, 0, res, 0, vd.Format);
                else
                    ((void(WINAPI *)(void *, void *, u32, u32, u32, u32, void *, u32, const void *))VT(ctx)[CopySubresourceRegion])(
                        ctx, G.copy, 0, 0, 0, 0, res, mip, 0);
                *w = cw; *h = ch; ok = 1;
            }
        }
    }
    rel(res); rel(rtv);
    return ok;
}

static float frac(double x) {
    if (x > 2.0e9 || x < -2.0e9) return 0;
    double f = x - (double)(int)x;
    return (float)(f < 0 ? f + 1 : f);
}
static int finite(float x) { return x == x && x - x == 0; }
static float vlen(float x, float y, float z) { return __builtin_sqrtf(x * x + y * y + z * z); }

/* row vector times matrix: o = (x, y, z, 1) * m */
static void xform(float *o, const float *m, float x, float y, float z) {
    for (int c = 0; c < 4; c++) o[c] = x * m[c] + y * m[4 + c] + z * m[8 + c] + m[12 + c];
}

/* Xbox HeatShimmer constructor defaults (prototype 0xCC5C4): {scale, rate, perturb} per pass */
static const float k_defaults[12] = { 1, 1, 10, 10,   0, -0.87f, 0, 0.87f,   1, 1, 2, 2 };

static int fill_consts(Consts *k, const u8 *self, const float *world, u32 w, u32 h, int *greater) {
    const float *V = (const float *)(G_DEV + DEV_VIEW), *P = (const float *)(G_DEV + DEV_PROJ);
    float c[4]; xform(c, V, world[12], world[13], world[14]);
    float dist = vlen(c[0], c[1], c[2]);
    float r = *(const float *)(self + HS_RADIUS);
    if (!finite(dist) || dist < 1e-4f || !finite(r) || r <= 0) return 0;

    /* the Xbox quad: corners 2r from the centre, so half a side is sqrt(2) r */
    float hs = 1.41421356f * r;
    for (int i = 0; i < 4; i++) {
        float dx = (i & 1) ? hs : -hs, dy = (i & 2) ? -hs : hs, o[4];
        float v[3] = { c[0] + dx, c[1] + dy, c[2] };
        for (int col = 0; col < 4; col++) o[col] = v[0] * P[col] + v[1] * P[4 + col] + v[2] * P[8 + col] + P[12 + col];
        if (!finite(o[0]) || !finite(o[1]) || !finite(o[2]) || !finite(o[3])) return 0;
        memcpy(k->corner[i], o, sizeof o);
    }
    /* which way the depth test goes: compare the depth of two points ahead of the camera */
    float zn = (1.0f * P[10] + P[14]) / (1.0f * P[11] + P[15]), zf = (100.0f * P[10] + P[14]) / (100.0f * P[11] + P[15]);
    *greater = zf < zn;

    float prm[12]; int any = 0;
    memcpy(prm, self + HS_SCALE, 16); memcpy(prm + 4, self + HS_RATE, 16); memcpy(prm + 8, self + HS_PERTURB, 16);
    for (int i = 0; i < 12; i++) { if (!finite(prm[i])) any = -1000; else if (prm[i] != 0) any++; }
    if (any <= 0) memcpy(prm, k_defaults, sizeof prm);   /* no shimmerpass records: the fields stay zero */

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double t = (double)(now.q - g_qpc0) * g_qpcScale, rsq = 1.0 / dist;
    float *n[2] = { k->noise0, k->noise1 };
    for (int p = 0; p < 2; p++) {
        n[p][0] = (float)(prm[2 * p] * rsq); n[p][1] = (float)(prm[2 * p + 1] * rsq);
        n[p][2] = frac(rsq * prm[4 + 2 * p] * t); n[p][3] = frac(rsq * prm[5 + 2 * p] * t);
    }
    /* The Xbox bent the 640x480 back buffer lookup by perturb[1] pixels; keep the same share of the screen. */
    float px = (float)h / 480.0f * g_strength;
    k->perturb[0] = prm[8]; k->perturb[1] = prm[9]; k->perturb[2] = prm[10] * px; k->perturb[3] = prm[11] * px;
    k->screen[0] = 1.0f / (float)w; k->screen[1] = 1.0f / (float)h; k->screen[2] = 1; k->screen[3] = g_debug ? 1.0f : 0.0f;
    return 1;
}

typedef struct {
    void *il, *vs, *gs, *ps, *vscb, *pscb, *srv[2], *smp[2], *blend, *depth, *raster;
    void *vsi[256], *gsi[256], *psi[256]; u32 nvsi, ngsi, npsi;
    u32 topo, mask, sref; float factor[4];
} Saved;
static Saved g_sv;

static void save_state(void *ctx, Saved *s) {
    memset(s, 0, sizeof *s);
    s->nvsi = s->ngsi = s->npsi = 256;
    CALL(ctx, IAGetInputLayout, Get1_t)(ctx, &s->il);
    ((void(WINAPI *)(void *, u32 *))VT(ctx)[IAGetPrimitiveTopology])(ctx, &s->topo);
    CALL(ctx, VSGetShader, GetSh_t)(ctx, &s->vs, s->vsi, &s->nvsi);
    CALL(ctx, GSGetShader, GetSh_t)(ctx, &s->gs, s->gsi, &s->ngsi);
    CALL(ctx, PSGetShader, GetSh_t)(ctx, &s->ps, s->psi, &s->npsi);
    CALL(ctx, VSGetConstantBuffers, GetN_t)(ctx, 0, 1, &s->vscb);
    CALL(ctx, PSGetConstantBuffers, GetN_t)(ctx, 0, 1, &s->pscb);
    CALL(ctx, PSGetShaderResources, GetN_t)(ctx, 0, 2, s->srv);
    CALL(ctx, PSGetSamplers, GetN_t)(ctx, 0, 2, s->smp);
    ((void(WINAPI *)(void *, void **, float *, u32 *))VT(ctx)[OMGetBlendState])(ctx, &s->blend, s->factor, &s->mask);
    ((void(WINAPI *)(void *, void **, u32 *))VT(ctx)[OMGetDepthStencilState])(ctx, &s->depth, &s->sref);
    CALL(ctx, RSGetState, Get1_t)(ctx, &s->raster);
}

static void restore_state(void *ctx, Saved *s) {
    CALL(ctx, IASetInputLayout, Set1_t)(ctx, s->il);
    ((void(WINAPI *)(void *, u32))VT(ctx)[IASetPrimitiveTopology])(ctx, s->topo);
    CALL(ctx, VSSetShader, SetSh_t)(ctx, s->vs, s->vsi, s->nvsi);
    CALL(ctx, GSSetShader, SetSh_t)(ctx, s->gs, s->gsi, s->ngsi);
    CALL(ctx, PSSetShader, SetSh_t)(ctx, s->ps, s->psi, s->npsi);
    CALL(ctx, VSSetConstantBuffers, SetN_t)(ctx, 0, 1, &s->vscb);
    CALL(ctx, PSSetConstantBuffers, SetN_t)(ctx, 0, 1, &s->pscb);
    CALL(ctx, PSSetShaderResources, SetN_t)(ctx, 0, 2, s->srv);
    CALL(ctx, PSSetSamplers, SetN_t)(ctx, 0, 2, s->smp);
    ((void(WINAPI *)(void *, void *, const float *, u32))VT(ctx)[OMSetBlendState])(ctx, s->blend, s->factor, s->mask);
    ((void(WINAPI *)(void *, void *, u32))VT(ctx)[OMSetDepthStencilState])(ctx, s->depth, s->sref);
    CALL(ctx, RSSetState, Set1_t)(ctx, s->raster);
    void **all[] = { &s->il, &s->vs, &s->gs, &s->ps, &s->vscb, &s->pscb, &s->srv[0], &s->srv[1], &s->smp[0],
                     &s->smp[1], &s->blend, &s->depth, &s->raster };
    for (u32 i = 0; i < sizeof all / sizeof all[0]; i++) rel(*all[i]);
    for (u32 i = 0; i < s->nvsi && i < 256; i++) rel(s->vsi[i]);
    for (u32 i = 0; i < s->ngsi && i < 256; i++) rel(s->gsi[i]);
    for (u32 i = 0; i < s->npsi && i < 256; i++) rel(s->psi[i]);
}

static int g_late = 1;

/* ---- diagnostics: one log line the first time each shimmer node is drawn ---- */
static u8 *g_seen[64]; static u32 g_nseen;
static void put(char *m, u32 *n, const char *t) { while (*t && *n < 250) m[(*n)++] = *t++; }
static void putd(char *m, u32 *n, float f) {   /* signed, 2 decimals */
    if (!finite(f)) { put(m, n, "nan"); return; }
    if (f < 0) { put(m, n, "-"); f = -f; }
    if (f > 1e7f) f = 1e7f;
    u32 v = (u32)(f * 100.0f + 0.5f), i = v / 100, d = v % 100; char b[16]; int k = 0;
    do { b[k++] = (char)('0' + i % 10); i /= 10; } while (i);
    while (k) { char c[2] = { b[--k], 0 }; put(m, n, c); }
    char c[4] = { '.', (char)('0' + d / 10), (char)('0' + d % 10), 0 }; put(m, n, c);
}
static void putu(char *m, u32 *n, u32 v) { char b[12]; int k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) { char c[2] = { b[--k], 0 }; put(m, n, c); } }
static void put_name(char *m, u32 *n, const u8 *node) {
    if (!node) { put(m, n, "-"); return; }
    u32 size = *(const u32 *)(node + 0x18), cap = *(const u32 *)(node + 0x1C);
    const char *p = cap >= 16 ? *(const char *const *)(node + 8) : (const char *)(node + 8);
    if (!p || size > 40 || cap < size) { put(m, n, "?"); return; }
    for (u32 i = 0; i < size; i++) { char c[2] = { p[i] >= 32 && p[i] < 127 ? p[i] : '?', 0 }; put(m, n, c); }
}
static void diagnose(void *ctx, u8 *self, const float *world, u32 w, u32 h, int greater) {
    for (u32 i = 0; i < g_nseen; i++) if (g_seen[i] == self) return;
    if (g_nseen < 64) g_seen[g_nseen++] = self; else return;
    char m[256]; u32 n = 0;
    float vp[6] = { 0 }; u32 nvp = 1;
    ((void(WINAPI *)(void *, u32 *, float *))VT(ctx)[RSGetViewports])(ctx, &nvp, vp);
    void *ds = 0; u32 ref = 0, dd[13] = { 0 };
    ((void(WINAPI *)(void *, void **, u32 *))VT(ctx)[OMGetDepthStencilState])(ctx, &ds, &ref);
    if (ds) { CALL(ds, 7, Set1_t)(ds, dd); rel(ds); }
    const float *V = (const float *)(G_DEV + DEV_VIEW);
    float c[4]; xform(c, V, world[12], world[13], world[14]);
    const u8 *parent = *(u8 *const *)(self + 0x20);
    put(m, &n, "shimmer: node "); put_name(m, &n, self); put(m, &n, " parent "); put_name(m, &n, parent);
    put(m, &n, " at "); putd(m, &n, world[12]); put(m, &n, " "); putd(m, &n, world[13]); put(m, &n, " "); putd(m, &n, world[14]);
    put(m, &n, " r "); putd(m, &n, *(const float *)(self + HS_RADIUS));
    put(m, &n, " dist "); putd(m, &n, vlen(c[0], c[1], c[2]));
    put(m, &n, " | target "); putu(m, &n, w); put(m, &n, "x"); putu(m, &n, h);
    put(m, &n, " depth "); putu(m, &n, g_dsW); put(m, &n, "x"); putu(m, &n, g_dsH);
    put(m, &n, " vp z "); putd(m, &n, vp[4]); put(m, &n, ".."); putd(m, &n, vp[5]);
    put(m, &n, greater ? " test GE" : " test LE");
    put(m, &n, " | game depth on "); putu(m, &n, dd[0]); put(m, &n, " func "); putu(m, &n, dd[2]);
    m[n] = 0; logs(m);
}

static void draw_one(u8 *self, const float *world) {
    u8 *gd = G_DEV; if (!gd) return;
    void *ctx = *(void **)(gd + DEV_CTX); if (!ctx) return;

    void *dev = 0; CALL(ctx, CTX_GetDevice, Get1_t)(ctx, &dev);
    if (!dev) return;
    if (dev != G.dev) {
        if (G.dev) logs("shimmer: the device changed, making the D3D11 objects again");
        release_all();
        if (!create_all(dev)) { release_all(); G.failed = 1; rel(dev); return; }
    }
    rel(dev);

    u32 w = 0, h = 0; int greater = 0; Consts k;
    if (!copy_target(ctx, &w, &h)) { if (!g_skipped++) logs("shimmer: skipped, the render target is not a 2D texture"); return; }
    if (!fill_consts(&k, self, world, w, h, &greater)) return;
    diagnose(ctx, self, world, w, h, greater);

    save_state(ctx, &g_sv);
    ((void(WINAPI *)(void *, void *, u32, const void *, const void *, u32, u32))VT(ctx)[UpdateSubresource])(ctx, G.cb, 0, 0, &k, 0, 0);
    CALL(ctx, IASetInputLayout, Set1_t)(ctx, 0);
    ((void(WINAPI *)(void *, u32))VT(ctx)[IASetPrimitiveTopology])(ctx, 5 /*TRIANGLESTRIP*/);
    CALL(ctx, VSSetShader, SetSh_t)(ctx, G.vs, 0, 0);
    CALL(ctx, GSSetShader, SetSh_t)(ctx, 0, 0, 0);
    CALL(ctx, PSSetShader, SetSh_t)(ctx, G.ps, 0, 0);
    CALL(ctx, VSSetConstantBuffers, SetN_t)(ctx, 0, 1, &G.cb);
    CALL(ctx, PSSetConstantBuffers, SetN_t)(ctx, 0, 1, &G.cb);
    void *srv[2] = { G.noiseSRV, G.copySRV }, *smp[2] = { G.wrap, G.clamp };
    CALL(ctx, PSSetShaderResources, SetN_t)(ctx, 0, 2, srv);
    CALL(ctx, PSSetSamplers, SetN_t)(ctx, 0, 2, smp);
    ((void(WINAPI *)(void *, void *, const float *, u32))VT(ctx)[OMSetBlendState])(ctx, G.blend, 0, 0xFFFFFFFF);
    ((void(WINAPI *)(void *, void *, u32))VT(ctx)[OMSetDepthStencilState])(ctx, greater ? G.depthGE : G.depthLE, 0);
    CALL(ctx, RSSetState, Set1_t)(ctx, G.raster);
    ((void(WINAPI *)(void *, u32, u32))VT(ctx)[Draw])(ctx, 4, 0);
    restore_state(ctx, &g_sv);
    if (!g_drawn++) logx("shimmer: first draw, target height", h);
}

/* Shimmers queued by the alpha pass, drawn at the end of the scene render (late=1). */
static struct { u8 *node; float world[16]; } g_queue[64];
static u32 g_nqueue;

/* HeatShimmer vtable slot 7: Render(const Matrix &world), thiscall, ret 4 */
static void __fastcall hs_render(u8 *self, void *edx, const float *world) {
    (void)edx;
    if (!g_enabled || G.failed || !self || !world) return;
    u8 *gd = G_DEV; if (!gd) return;
    /* The alpha list is drawn twice (0x4A5CD0): "OpaqueNodesInAlphaNode" (selector 2, 0x4A36E6) with
       alpha test and depth writes, then "AlphaNodes" (selector 1) blended without depth writes. Queued
       shimmers are kept once per node per frame, whichever pass reached them. Drawing at once (late=0),
       the cached depth-write state picks the blended pass; other nodes' draws can leave it set, so a
       shimmer may miss a frame there. */
    if (!g_late) {
        if (*(const u32 *)(gd + DEV_ZWRITE) == 0) draw_one(self, world);
        return;
    }
    for (u32 i = 0; i < g_nqueue; i++)
        if (g_queue[i].node == self) { memcpy(g_queue[i].world, world, 64); return; }
    if (g_nqueue < 64) { g_queue[g_nqueue].node = self; memcpy(g_queue[g_nqueue].world, world, 64); g_nqueue++; }
}

/* End of the scene render (0x4A3DA9): every world and node pass is done, the scene target and its depth
   are still bound, and the render-scale composite (0x4641F0) has not run yet. */
__attribute__((used)) void hs_flush(void) {
    u32 n = g_nqueue; g_nqueue = 0;
    for (u32 i = 0; i < n; i++)
        if (*(const u32 *)g_queue[i].node == R(HS_VTABLE)) draw_one(g_queue[i].node, g_queue[i].world);
}

/* Replaces `test dword ptr [ebx+0xF4], 0x30000` (10 bytes) at 0x4A3DA9 with a call here and 5 nops. */
__attribute__((naked)) static void hs_flush_stub(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call _hs_flush\n\t"
        "popal\n\t"
        "testl $0x30000, 0xF4(%ebx)\n\t"
        "retl\n\t");
}

/* Constructor tail: the Xbox constructor bounded the node by +-radius; the remaster's does not.
   The node also skips the remaster's occlusion queries: past 10 m the alpha pass draws a small node
   only when last frame's query on its bounds saw pixels (0x4A0280), and a torch flame's +-0.25 m box
   is mostly inside the torch, so the shimmer blinked on and off while the camera turned. The depth
   test still hides it behind walls. */
__attribute__((used)) void hs_after_ctor(u8 *self, const float *radius) {
    if (!self) return;
    if (radius) SetEllipsoidRadius(self, radius);
    SetOption(self, OPT_NO_OCCLUSION, 1, 0);
}

/* Replaces `pop esi; mov esp,ebp; pop ebp; ret 4` at 0x45E27F (ESI = this, [EBP+8] = radius). */
__attribute__((naked)) static void hs_ctor_tail(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushl 8(%ebp)\n\t"
        "pushl %esi\n\t"
        "call _hs_after_ctor\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popl %esi\n\t"
        "movl %ebp, %esp\n\t"
        "popl %ebp\n\t"
        "retl $4\n\t");
}

#include "sites.h"

static int check_sites(void) {
    int ok = 1;
    for (u32 i = 0; i < sizeof g_sites / sizeof g_sites[0]; i++) {
        const u8 *p = (const u8 *)R(g_sites[i].va);
        for (u32 k = 0; k < sizeof g_sites[i].b; k++)
            if ((p[k] & g_sites[i].m[k]) != (g_sites[i].b[k] & g_sites[i].m[k])) {
                logx("check failed:", g_sites[i].va); logs(g_sites[i].name); ok = 0; break;
            }
    }
    /* the vtable is all relocated pointers, so it is checked here instead */
    const u32 *vt = (const u32 *)R(HS_VTABLE);
    if (vt[0] != R(HS_DTOR) || vt[1] != R(HS_BOUNDS) || vt[7] != R(HS_NORENDER)) {
        logx("check failed: HeatShimmer vtable slot 7 =", vt[7]); ok = 0;
    } else {
        const u8 *name = (const u8 *)(*(const u32 *)(*(const u32 *)((const u8 *)vt - 4) + 12) + 8);
        const char *want = ".?AVHeatShimmer@@";
        for (u32 i = 0; want[i] || name[i]; i++)
            if (name[i] != (u8)want[i]) { logs("check failed: vtable 0xAD9440 is not HeatShimmer's"); ok = 0; break; }
    }
    return ok;
}

static int write_code(u32 va, const u8 *w, u32 n) {
    u8 *site = (u8 *)R(va); u32 old;
    if (!VirtualProtect(site, n, 0x40, &old)) return 0;
    memcpy(site, w, n);
    VirtualProtect(site, n, old, &old); FlushInstructionCache(GetCurrentProcess(), site, n);
    return 1;
}

#ifdef VINCE_AIO /* 1.4 T7: the settings are [VinceSteam] of the AIO core's VinceAIO.ini */
#define VS_INI_NAME "VinceAIO.ini"
#else
#define VS_INI_NAME "VinceSteam.ini"
#endif
static void install(void) {
    if (!check_sites()) { logs("VinceSteam is OFF: this Vince.exe is not the one it was built for"); return; }
    u8 jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    u32 rel32 = (u32)hs_ctor_tail - (R(0x45E27F) + 5); memcpy(jmp + 1, &rel32, 4);
    u32 slot = (u32)hs_render;
    u8 call[10] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x44, 0x00, 0x00 };   /* call; 5-byte nop */
    rel32 = (u32)hs_flush_stub - (R(0x4A3DA9) + 5); memcpy(call + 1, &rel32, 4);
    if (!write_code(0x45E27F, jmp, sizeof jmp) || !write_code(HS_VTABLE + 7 * 4, (const u8 *)&slot, 4) ||
        !write_code(0x4A3DA9, call, sizeof call)) {
        logs("VinceSteam is OFF: VirtualProtect failed"); return;
    }
    logs(g_enabled ? "VinceSteam 7 ready: heat shimmer on" : "VinceSteam 7 ready: heat shimmer turned off in " VS_INI_NAME);
}

#ifdef VINCE_AIO
/* AIO core (aio/core/core.c, 1.4 T6): no exports, no DllMain; the core calls this body of DllMain on a host whose
   SizeOfImage is at least 0x87A000, after its own DisableThreadLibraryCalls. 1.4 T7: ini = the core's VinceAIO.ini,
   section [VinceSteam] ("" or null: the defaults stand). */
void vincesteam_attach(const WCHAR *ini) {
#else
/* The exe imports this so that Windows loads the DLL; it is never called. */
__declspec(dllexport) int VinceSteamBootstrap(void) { return 0; }

BOOL WINAPI DllMain(HMODULE h, u32 reason, void *r) {
    (void)r;
    if (reason != 1) return 1;
    DisableThreadLibraryCalls(h);
#endif
    D =(u32)GetModuleHandleA(0) - 0x400000;
    /* 0 = failed, 520 = truncated, no '\\' = no folder: then no log and no ini (the defaults stand). */
    WCHAR dir[520]; u32 n = GetModuleFileNameW(0, dir, 520);
    if (n >= 520) n = 0;
    while (n && dir[n - 1] != L'\\') n--;
    u32 pct = 100;
    if (n) {
        memcpy(g_logpath, dir, 2 * n); memcpy(g_logpath + n, L"VinceSteam.log", 2 * 15);
#ifdef VINCE_AIO
    }
    if (ini && ini[0]) {
#define VS_SECTION L"VinceSteam"
#else
        WCHAR ini[520 + 16]; memcpy(ini, dir, 2 * n); memcpy(ini + n, L"VinceSteam.ini", 2 * 15);
#define VS_SECTION L"shimmer"
#endif
        g_enabled = (int)GetPrivateProfileIntW(VS_SECTION, L"enabled", 1, ini) != 0;
        pct = GetPrivateProfileIntW(VS_SECTION, L"strength", 100, ini);
        g_debug = (int)GetPrivateProfileIntW(VS_SECTION, L"debug", 0, ini) != 0;
        g_late = (int)GetPrivateProfileIntW(VS_SECTION, L"late", 1, ini) != 0;
    }
    logs(g_late ? "late=1: shimmers are drawn at the end of the scene" : "late=0: shimmers are drawn in the alpha pass");
    if (g_debug) logs("debug=1: shimmers are drawn as red discs");
    g_strength = (float)(pct > 1000 ? 1000 : pct) / 100.0f;
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    g_qpcScale = f.q ? 1.0 / (double)f.q : 0; g_qpc0 = c.q;
    logx("---- VinceSteam start, strength percent", pct);
    install();
#ifndef VINCE_AIO
    return 1;
#endif
}
