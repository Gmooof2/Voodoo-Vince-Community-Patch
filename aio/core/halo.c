/*
 * AIO core (1.6 T4): VoodooPowerFix's legacy pickup halo, always on. Runtime ported from
 * VoodooPowerFix/pickup_halo_toggle.c:1-169 (that file stays the pinned v2 copy): the memory guard, halo_* functions and
 * halo_install with its 5 byte checks and rollback order. Dropped: pickup_tick, its toggle key poll, toast/debounce state;
 * g_pickupOn stays 1. VinceDebug's helpers (R, ckh, put_rel, patch, logs) are replaced by the core adapters below.
 * Installed once from DllMain (core.c step 6), only when Vince.exe's SpookyPS9 bytecode is VoodooPowerFix's halo blur.
 * Engine owns node+144 throughout node lifetime; wrappers borrow it only for synchronous rendering. No node pointers
 * are retained between calls.
 */
#include "kernel32.h"

void *memcpy(void *d, const void *s, unsigned n); /* vincesteam.c */
void *memset(void *d, int c, unsigned n);         /* VinceAudio bridge_engine.c */
extern unsigned long g_exe_base;                  /* VinceFix: Vince.exe's load address, set by core.c step 3 */
void log_text(const char *text);                  /* VinceFix's log */

#define SPOOKY_RVA 0x386308u /* SpookyPS9 bytecode (file 0x385308), VPF build_pickup_halo.PATCHED */
#define SPOOKY_LEN 45292u
static const u8 SPOOKY_SHA[32] = { /* aio/components.json VoodooPowerFix SpookyPS9 pin (T1) */
    0x7f,0x63,0x96,0xac,0x9c,0x28,0x8b,0x5f,0x3b,0x11,0xbb,0x87,0xbd,0x7a,0x5a,0x42,
    0x04,0x23,0xe1,0x89,0xb3,0x33,0x05,0x7e,0xda,0x19,0x4a,0x03,0x8f,0xc9,0x2e,0x46};

/* ---- core adapters (VinceDebug's R/ckh/put_rel/patch) ---- */
static u32 R(u32 va) { return (u32)g_exe_base + va - 0x400000u; }

typedef struct { u32 base, allocation, allocation_protect, size, state, protect, type; } PickupMemoryInfo;
static int pickup_memory_ok(const void *p, u32 n, int write) {
    u32 start=(u32)p, end=start+n;
    if (!start || !n || end<start) return 0;
    while(start<end) {
        PickupMemoryInfo m; u32 next;
        if(VirtualQuery((void*)start,&m,sizeof m)!=sizeof m || m.state!=0x1000 ||
           (m.protect&0x100) || (m.protect&0xff)==1) return 0;
        if(write && !(m.protect&(4|8|0x40|0x80))) return 0;
        if(!write && !(m.protect&(2|4|8|0x20|0x40|0x80))) return 0;
        next=m.base+m.size; if(next<=start) return 0; start=next;
    }
    return 1;
}
#define PICKUP_MEMORY_OK(p,n,w) pickup_memory_ok(p,n,w)

static int hexval(char c) { return c>='0'&&c<='9' ? c-'0' : c>='A'&&c<='F' ? c-'A'+10 : c>='a'&&c<='f' ? c-'a'+10 : -1; }
/* 1 when the bytes at exe VA va are readable and equal the hex string. */
static int ckh(u32 va, const char *hex) {
    const u8 *p=(const u8*)R(va); u32 n=0,i;
    while(hex[2*n]) n++;
    if(!PICKUP_MEMORY_OK(p,n,0)) return 0;
    for(i=0;i<n;i++) if(p[i]!=(u8)(hexval(hex[2*i])*16+hexval(hex[2*i+1]))) return 0;
    return 1;
}
/* b[0] is the E8/E9 opcode at address from; its rel32 reaches to. */
static void put_rel(u8 *b, u32 from, void *to) {
    u32 rel=(u32)to-from-5; b[1]=(u8)rel; b[2]=(u8)(rel>>8); b[3]=(u8)(rel>>16); b[4]=(u8)(rel>>24);
}
/* Refuses unless site holds before; as VinceSteam/src/vincesteam.c:523-529 write_code. */
static int patch(u8 *site, const u8 *before, const u8 *after, u32 n) {
    u32 old,i;
    if(!PICKUP_MEMORY_OK(site,n,0)) return 0;
    for(i=0;i<n;i++) if(site[i]!=before[i]) return 0;
    if(!VirtualProtect(site,n,0x40,&old)) return 0;
    memcpy(site,after,n);
    VirtualProtect(site,n,old,&old); FlushInstructionCache(GetCurrentProcess(),site,n);
    return 1;
}

/* ---- the halo runtime (pickup_halo_toggle.c:24-169) ---- */
static int g_pickupOn=1, g_haloBusy, g_haloActive;
typedef void (__thiscall *HaloRender4)(u8*,void*,void*,void*,u32);
typedef void (__thiscall *HaloRender1)(u8*,void*);
typedef void (__thiscall *HaloSetSuper)(u8*,u32,u32,u32,float,int);
typedef u8 *(__thiscall *HaloTarget)(u8*,u32,u32,u32,u32);
typedef void (__thiscall *HaloQuad)(u8*,u32,u32,const void*,u32);
/* One block: tramp4 at +0, tramp1 at +32; written RW, then PAGE_EXECUTE_READ. */
static u8 *g_haloTramp;
static HaloRender4 g_haloRender4;
static HaloRender1 g_haloRender1;
static int halo_model_name(u8 *model) {
    u32 size,cap,i,j; const u8 *s; const char *key="powericon_base";
    if(!PICKUP_MEMORY_OK(model,0x1c,0)) return 0;
    size=*(u32*)(model+0x14); cap=*(u32*)(model+0x18);
    if(size>cap || size<14 || size>1024 || cap>0x1000000) return 0;
    s=cap<16 ? model+4 : *(u8**)(model+4);
    if(!PICKUP_MEMORY_OK(s,size,0)) return 0;
    for(i=0;i+14<=size;i++) {
        for(j=0;j<14;j++) { u8 c=s[i+j]; if(c>='A'&&c<='Z') c+=32; if(c!=key[j]) break; }
        if(j==14) return 1;
    }
    return 0;
}
static int halo_pickup(u8 *node) {
    if(!PICKUP_MEMORY_OK(node,0x148,1)) return 0;
    return *(u32*)(node+0x128)==0x3f32b021 && *(u32*)(node+0x12c)==0x3e9eb852 &&
           *(u32*)(node+0x130)==0x3f66e979 && halo_model_name(*(u8**)(node+0xb8));
}
typedef struct { u32 flags,opacity,mode; u8 ca; u8 *effect; } HaloSave;
static HaloSave halo_save(u8 *n) {
    HaloSave s; s.flags=*(u32*)(n+0x88); s.opacity=*(u32*)(n+0x8c);
    s.mode=*(u32*)(n+0xd0); s.ca=n[0xca]; s.effect=*(u8**)(n+0x144); return s;
}
static void halo_restore(u8 *n,HaloSave s) {
    *(u32*)(n+0x88)=s.flags; *(u32*)(n+0x8c)=s.opacity;
    *(u32*)(n+0xd0)=s.mode; n[0xca]=s.ca; *(u8**)(n+0x144)=s.effect;
}
static int halo_owned(u8 *p) {
    return PICKUP_MEMORY_OK(p,0x18,1) && *(u32*)(p+8)==0x2b2b2b &&
           *(u32*)p==640 && *(u32*)(p+4)==480;
}
static int halo_prepare(u8 *n,HaloSave *s) {
    *s=halo_save(n);
    if(!s->effect && g_pickupOn) {
        ((HaloSetSuper)R(0x4393e0))(n,640,480,0x2b2b2b,1.0f,1);
        s->effect=*(u8**)(n+0x144);
        halo_restore(n,*s); /* Retain new owned effect, undo helper's flags. */
    }
    return halo_owned(s->effect);
}
static u8 *__fastcall halo_target(u8 *renderer,void *unused,u32 w,u32 h,u32 depth,u32 flags) {
    (void)unused;
    if(g_haloActive) { w=640; h=480; depth=480; }
    return ((HaloTarget)R(0x465ef0))(renderer,w,h,depth,flags);
}
static void __fastcall halo_quad(u8 *renderer,void *unused,u32 primitive,u32 count,const void *vertices,u32 format) {
    u8 marked[6*24]; u32 i; (void)unused;
    if(g_haloActive && primitive==4 && count==6 && format==0x1c && PICKUP_MEMORY_OK(vertices,sizeof marked,0)) {
        memcpy(marked,vertices,sizeof marked);
        for(i=0;i<6;i++) {
            u32 *color=(u32*)(marked+i*24+12); *color=(*color&0xff000000)|0x2b2b2b;
        }
        ((HaloQuad)R(0x45f670))(renderer,primitive,count,marked,format);
    } else ((HaloQuad)R(0x45f670))(renderer,primitive,count,vertices,format);
}
static void __fastcall halo_render4(u8 *n,void *unused,void *a,void *b,void *c,u32 pass) {
    HaloSave s; (void)unused;
    if(g_haloBusy || !halo_pickup(n)) { g_haloRender4(n,a,b,c,pass); return; }
    if(!halo_prepare(n,&s)) { g_haloRender4(n,a,b,c,pass); return; }
    g_haloBusy=1;
    /* Composite before this model writes depth; scene occlusion stays enabled. */
    if(g_pickupOn) {
        *(u8**)(n+0x144)=s.effect; *(u32*)(n+0x8c)=0x437f0000;
        n[0xca]=0; *(u32*)(n+0xd0)=0;
        g_haloActive=1; g_haloRender4(n,a,b,c,pass); g_haloActive=0;
    }
    halo_restore(n,s); *(u8**)(n+0x144)=0;
    if(g_pickupOn) { n[0xca]=0; *(u32*)(n+0xd0)=0; }
    g_haloRender4(n,a,b,c,pass);
    halo_restore(n,s); g_haloBusy=0;
}
static void __fastcall halo_render1(u8 *n,void *unused,void *view) {
    HaloSave s; (void)unused;
    if(g_haloBusy || !halo_pickup(n)) { g_haloRender1(n,view); return; }
    if(!halo_prepare(n,&s)) { g_haloRender1(n,view); return; }
    g_haloBusy=1;
    if(g_pickupOn) {
        *(u8**)(n+0x144)=s.effect; *(u32*)(n+0x8c)=0x437f0000;
        n[0xca]=1; *(u32*)(n+0xd0)=0;
        g_haloActive=1; g_haloRender1(n,view); g_haloActive=0;
    }
    halo_restore(n,s); *(u8**)(n+0x144)=0;
    /* Native mode 1 skips the model; mode 0 with no effect draws it. */
    if(g_pickupOn) { n[0xca]=0; *(u32*)(n+0xd0)=0; }
    g_haloRender1(n,view);
    halo_restore(n,s); g_haloBusy=0;
}
static void halo_trampoline(u8 *target,u32 len,u8 *out) {
    memcpy(out,target,len); out[len]=0xe9; put_rel(out+len,(u32)out+len,target+len);
}
static int halo_release(void) {
    VirtualFree(g_haloTramp,0,0x8000); /* MEM_RELEASE */
    g_haloTramp=0; g_haloRender4=0; g_haloRender1=0; return 0;
}
static int halo_install(void) {
    u8 before4[6],before1[9],beforeCall[5],beforeQuad[5],hook4[6],hook1[9],hookCall[5],hookQuad[5];
    u32 old;
    if(!ckh(0x436df0,"558BEC83E4F8") ||
       !ckh(0x437890,"558BEC64A100000000") ||
       !ckh(0x4393e0,"558BEC51568BF18B8644010000") ||
       !ckh(0x48ca2f,"E8BC94FDFF") ||
       !ckh(0x48d11a,"E85125FDFF")) return 0;
    memcpy(before4,(void*)R(0x436df0),6); memcpy(before1,(void*)R(0x437890),9);
    memcpy(beforeCall,(void*)R(0x48ca2f),5);
    memcpy(beforeQuad,(void*)R(0x48d11a),5);
    g_haloTramp=(u8*)VirtualAlloc(0,64,0x3000,4); /* MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE */
    if(!g_haloTramp) return 0;
    halo_trampoline((u8*)R(0x436df0),6,g_haloTramp);
    halo_trampoline((u8*)R(0x437890),9,g_haloTramp+32);
    if(!VirtualProtect(g_haloTramp,64,0x20,&old)) return halo_release(); /* PAGE_EXECUTE_READ */
    FlushInstructionCache(GetCurrentProcess(),g_haloTramp,64);
    g_haloRender4=(HaloRender4)g_haloTramp; g_haloRender1=(HaloRender1)(g_haloTramp+32);
    memset(hook4,0x90,6); hook4[0]=0xe9; put_rel(hook4,R(0x436df0),halo_render4);
    memset(hook1,0x90,9); hook1[0]=0xe9; put_rel(hook1,R(0x437890),halo_render1);
    hookCall[0]=0xe8; put_rel(hookCall,R(0x48ca2f),halo_target);
    hookQuad[0]=0xe8; put_rel(hookQuad,R(0x48d11a),halo_quad);
    /* Every failure path rolls back first, then frees the trampoline block. */
    if(!patch((u8*)R(0x48ca2f),beforeCall,hookCall,5)) return halo_release();
    if(!patch((u8*)R(0x48d11a),beforeQuad,hookQuad,5)) {
        patch((u8*)R(0x48ca2f),hookCall,beforeCall,5); return halo_release();
    }
    if(!patch((u8*)R(0x436df0),before4,hook4,6)) {
        patch((u8*)R(0x48d11a),hookQuad,beforeQuad,5);
        patch((u8*)R(0x48ca2f),hookCall,beforeCall,5); return halo_release();
    }
    if(!patch((u8*)R(0x437890),before1,hook1,9)) {
        patch((u8*)R(0x436df0),hook4,before4,6);
        patch((u8*)R(0x48d11a),hookQuad,beforeQuad,5);
        patch((u8*)R(0x48ca2f),hookCall,beforeCall,5); return halo_release();
    }
    return 1;
}

/* ---- SHA-256 (FIPS 180-4), for the SpookyPS9 identity check ---- */
static const u32 SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha_block(u32 h[8], const u8 *p) {
    u32 w[64],v[8],i,t1,t2;
    for(i=0;i<16;i++) w[i]=(u32)p[4*i]<<24|(u32)p[4*i+1]<<16|(u32)p[4*i+2]<<8|p[4*i+3];
    for(i=16;i<64;i++)
        w[i]=w[i-16]+(ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10));
    for(i=0;i<8;i++) v[i]=h[i];
    for(i=0;i<64;i++) {
        t1=v[7]+(ROR(v[4],6)^ROR(v[4],11)^ROR(v[4],25))+((v[4]&v[5])^(~v[4]&v[6]))+SHA_K[i]+w[i];
        t2=(ROR(v[0],2)^ROR(v[0],13)^ROR(v[0],22))+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]));
        v[7]=v[6]; v[6]=v[5]; v[5]=v[4]; v[4]=v[3]+t1; v[3]=v[2]; v[2]=v[1]; v[1]=v[0]; v[0]=t1+t2;
    }
    for(i=0;i<8;i++) h[i]+=v[i];
}
static void sha256(const u8 *p, u32 n, u8 out[32]) {
    u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8 last[128]; u32 i,r=n%64,tail;
    for(i=0;i+64<=n;i+=64) sha_block(h,p+i);
    memset(last,0,sizeof last); memcpy(last,p+n-r,r); last[r]=0x80;
    tail=r<56 ? 64 : 128; /* then the 64-bit big-endian bit length */
    last[tail-1]=(u8)(n<<3); last[tail-2]=(u8)(n>>5); last[tail-3]=(u8)(n>>13); last[tail-4]=(u8)(n>>21);
    last[tail-5]=(u8)(n>>29);
    sha_block(h,last); if(tail==128) sha_block(h,last+64);
    for(i=0;i<32;i++) out[i]=(u8)(h[i/4]>>(24-8*(i%4)));
}

/* core.c step 6: hook only when SpookyPS9 is VoodooPowerFix's halo blur (the shader the hooks feed); one log line. */
void halo_attach(u8 *image) {
    u8 got[32]; u32 i;
    if(!PICKUP_MEMORY_OK(image+SPOOKY_RVA,SPOOKY_LEN,0)) { log_text("halo: SpookyPS9 is not readable; halo off"); return; }
    sha256(image+SPOOKY_RVA,SPOOKY_LEN,got);
    for(i=0;i<32;i++) if(got[i]!=SPOOKY_SHA[i]) { log_text("halo: SpookyPS9 is not VoodooPowerFix's; halo off"); return; }
    log_text(halo_install() ? "halo: legacy pickup halo hooks ready" :
             "halo: compatibility check failed; hooks disabled");
}
