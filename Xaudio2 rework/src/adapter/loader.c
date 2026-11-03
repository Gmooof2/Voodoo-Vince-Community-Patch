/* This DLL is explicitly imported by the experimental EXE. DllMain only
 * redirects the game's existing CoCreateInstance import. Backend loading and
 * all audio work happen later, in the game's normal initialization call. */
#include "bridge_voice.h"
typedef struct {VaU32 a;VaU16 b,c;VaU8 d[8];} Guid;
VA_IMP void *VA_API GetModuleHandleW(const VaU16*);
VA_IMP int VA_API VirtualProtect(void*,VaU32,VaU32,VaU32*);
VA_IMP int VA_API DisableThreadLibraryCalls(void*);
VaHR VA_API VinceAudioCreate(void**);
void va_set_module(void*);
static VaHR(VA_API *real_com)(const Guid*,void*,VaU32,const Guid*,void**);
static const Guid clsid={0x5a508685,0xa254,0x4fba,{0x9b,0x82,0x9a,0x24,0xb0,3,6,0xaf}};
static const Guid iid={0x8bcf1f58,0x9fe7,0x4583,{0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb}};
static int same(const void*a,const void*b,unsigned n){const VaU8*x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static VaHR VA_API create(const Guid*c,void*outer,VaU32 context,const Guid*i,void**out){
    if(!outer && (context&1) && c && i && same(c,&clsid,16) && same(i,&iid,16))return VinceAudioCreate(out);
    return real_com(c,outer,context,i,out);
}
void VA_API VinceAudioBootstrap(void){} /* Load-time import; never called. */
int va_install(void *image){
    VaU8*base=image;
    if(!base || *(VaU16*)base!=0x5a4d)return 1;
    VaU32 pe=*(VaU32*)(base+0x3c);
    if(*(VaU32*)(base+pe)!=0x4550 || *(VaU16*)(base+pe+24)!=0x10b || *(VaU32*)(base+pe+24+56)<0x87a000)return 1;
    const VaU8 source_call[]={0xff,0x52,0x20,0x8b,0x4b,0x10};
    /* A large candidate with the wrong code must not quietly run the legacy
       backend and appear to be testing recovery. Small utility hosts above
       are allowed so the native smoke test can import the DLL normally. */
    if(!same(base+0x5cdbf,source_call,sizeof source_call))return 0;
    void **slot=(void**)(base+0x2d63d0);VaU32 old;
    if(*slot==(void*)create)return 1;
    if(!VirtualProtect(slot,4,4,&old))return 0;
    real_com=(void*)*slot;*slot=(void*)create;VirtualProtect(slot,4,old,&old);
    return 1;
}
#ifndef VINCE_AIO /* AIO core (aio/core/core.c, 1.4 T6) has the one DllMain: va_set_module(core), then va_install */
int VA_API DllMain(void*module,VaU32 reason,void*reserved){
    (void)reserved;if(reason!=1)return 1;
    va_set_module(module);DisableThreadLibraryCalls(module);
    return va_install(GetModuleHandleW(0));
}
#endif
