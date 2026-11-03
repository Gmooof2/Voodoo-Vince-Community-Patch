/* Native x86 ABI adapter: 2.7 voice objects backed by 2.9 voices. */
#include "bridge_shadow.h"
/* VaVoice (+ shadow rec at +12), VaVoiceState29: bridge_shadow.h; VaBuffer29: bridge_voice.h (2.9 records passed through raw, x86). */
/* Shadow hooks (T4) run after the real call; SH skips them with one relaxed load when the voice is not shadowed. */
#define SH(call) do{if(VA_SHADOWED(v))call;}while(0)
#define R(v) ((v)->real)
#define FN(v,n,T) ((T)(*(void ***)R(v))[n])
/* Outage Lost policy (1.4 T2): Lost or silent (real NULL) -> no real call, S_OK, the shadow hook still runs; getters
   answer from the shadow. A fatal real HRESULT signals Lost and becomes S_OK. Switch off: one relaxed load. */
/* 1.5 T1 in-flight guard: on an outage engine every path using v->real counts itself in engine calls before its Lost
   check and out after the real call (a rebuild attempt never frees a real in use); IN/OUT around LOST getters. */
#define CALL(v,name,call) (VA_OUTAGE()?({VaShadowEng*in_=va_sh_in(v);VaHR h_=va_sh_lost(v)?0:va_sh_failed(v,name,(call));va_sh_out(in_);h_;}):(call))
#define LOST(v) (VA_OUTAGE() && va_sh_lost(v))
#define IN(v) VaShadowEng*in_=VA_OUTAGE()?va_sh_in(v):0
#define OUT() va_sh_out(in_)
static void filter_default(void*a){if(a){((VaU32*)a)[0]=0;((float*)a)[1]=1.0f;((float*)a)[2]=1.0f;}} /* LowPass, 1.0, 1.0 */
void *va_unwrap_voice(void *legacy) { return legacy ? ((VaVoice*)legacy)->real : 0; }
VaHR va_translate_sends(const VaSends *in,VaSends *out,VaSend *storage,unsigned capacity) {
    unsigned i;
    if (!out || !in || in->SendCount>capacity || (in->SendCount && (!storage || !in->pSends))) return VA_E_INVALIDARG;
    out->SendCount=in->SendCount; out->pSends=storage;
    for(i=0;i<in->SendCount;++i) { storage[i].Flags=in->pSends[i].Flags; storage[i].pOutputVoice=va_unwrap_voice(in->pSends[i].pOutputVoice); }
    return 0;
}
static void VA_API details(VaVoice*v,VaDetails27*out) {
    typedef void(VA_API *F)(void*,VaDetails29*); VaDetails29 d;
    IN(v);if(LOST(v))va_sh_get_details(v,out);
    else{FN(v,0,F)(R(v),&d); out->CreationFlags=d.CreationFlags;out->InputChannels=d.InputChannels;out->InputSampleRate=d.InputSampleRate;}
    OUT();
    VA_TRACE(v,"GetVoiceDetails","out=%p channels=%u rate=%u -> void",out,out->InputChannels,out->InputSampleRate);
}
static VaHR set_sends(VaVoice*v,const VaSends*in) {
    typedef VaHR(VA_API *F)(void*,const VaSends*); VaSend *storage=0;VaSends translated;VaHR hr;
    if(!in)return CALL(v,"SetOutputVoices",FN(v,1,F)(R(v),0));
    if(in->SendCount>0x1fffffffUL)return VA_E_INVALIDARG;
    if(in->SendCount) { storage=HeapAlloc(GetProcessHeap(),0,in->SendCount*sizeof(VaSend));if(!storage)return VA_E_OUTOFMEMORY; }
    hr=va_translate_sends(in,&translated,storage,in->SendCount);
    if(hr>=0)hr=CALL(v,"SetOutputVoices",FN(v,1,F)(R(v),&translated));
    if(storage)HeapFree(GetProcessHeap(),0,storage);return hr;
}
static VaHR VA_API sends(VaVoice*v,const VaSends*in) {
    VaHR hr=set_sends(v,in);SH(va_sh_un(v,VA_UN_SetOutputVoices,hr));VA_TRACE(v,"SetOutputVoices","sends=%p count=%u -> 0x%x",in,in?in->SendCount:0UL,hr);return hr;
}
static VaHR VA_API effect_chain(VaVoice*v,const void*p) {
    typedef VaHR(VA_API*F)(void*,const void*);
    /* Legacy XAPOs need their own ABI adapter; this experiment has none. */
    VaHR hr=(p && *(const VaU32*)p)?VA_E_NOTIMPL:CALL(v,"SetEffectChain",FN(v,2,F)(R(v),p));
    SH(va_sh_un(v,VA_UN_SetEffectChain,hr));
    VA_TRACE(v,"SetEffectChain","chain=%p -> 0x%x",p,hr);return hr;
}
static VaHR VA_API enable(VaVoice*v,VaU32 a,VaU32 b) {typedef VaHR(VA_API*F)(void*,VaU32,VaU32);VaHR hr=CALL(v,"EnableEffect",FN(v,3,F)(R(v),a,b));SH(va_sh_un(v,VA_UN_EnableEffect,hr));VA_TRACE(v,"EnableEffect","index=%u operationSet=%u -> 0x%x",a,b,hr);return hr;}
static VaHR VA_API disable(VaVoice*v,VaU32 a,VaU32 b) {typedef VaHR(VA_API*F)(void*,VaU32,VaU32);VaHR hr=CALL(v,"DisableEffect",FN(v,4,F)(R(v),a,b));SH(va_sh_un(v,VA_UN_DisableEffect,hr));VA_TRACE(v,"DisableEffect","index=%u operationSet=%u -> 0x%x",a,b,hr);return hr;}
static void VA_API effect_state(VaVoice*v,VaU32 a,int*b) {typedef void(VA_API*F)(void*,VaU32,int*);IN(v);if(LOST(v)){if(b)*b=0;}else FN(v,5,F)(R(v),a,b);OUT();VA_TRACE(v,"GetEffectState","index=%u out=%p -> void",a,b);}
static VaHR VA_API effect_set(VaVoice*v,VaU32 a,const void*b,VaU32 c,VaU32 d) {typedef VaHR(VA_API*F)(void*,VaU32,const void*,VaU32,VaU32);VaHR hr=CALL(v,"SetEffectParameters",FN(v,6,F)(R(v),a,b,c,d));SH(va_sh_un(v,VA_UN_SetEffectParameters,hr));VA_TRACE(v,"SetEffectParameters","index=%u params=%p bytes=%u operationSet=%u -> 0x%x",a,b,c,d,hr);return hr;}
static VaHR VA_API effect_get(VaVoice*v,VaU32 a,void*b,VaU32 c) {typedef VaHR(VA_API*F)(void*,VaU32,void*,VaU32);VaHR hr=CALL(v,"GetEffectParameters",FN(v,7,F)(R(v),a,b,c));VA_TRACE(v,"GetEffectParameters","index=%u out=%p bytes=%u -> 0x%x",a,b,c,hr);return hr;}
static VaHR VA_API filter_set(VaVoice*v,const void*a,VaU32 b) {typedef VaHR(VA_API*F)(void*,const void*,VaU32);VaHR hr=CALL(v,"SetFilterParameters",FN(v,8,F)(R(v),a,b));SH(va_sh_un(v,VA_UN_SetFilterParameters,hr));VA_TRACE(v,"SetFilterParameters","params=%p operationSet=%u -> 0x%x",a,b,hr);return hr;}
static void VA_API filter_get(VaVoice*v,void*a) {typedef void(VA_API*F)(void*,void*);IN(v);if(LOST(v))filter_default(a);else FN(v,9,F)(R(v),a);OUT();VA_TRACE(v,"GetFilterParameters","out=%p -> void",a);}
static VaHR VA_API output_filter_set(VaVoice*v,void*a,const void*b,VaU32 c) {typedef VaHR(VA_API*F)(void*,void*,const void*,VaU32);VaHR hr=CALL(v,"SetOutputFilterParameters",FN(v,10,F)(R(v),va_unwrap_voice(a),b,c));SH(va_sh_un(v,VA_UN_SetOutputFilterParameters,hr));VA_TRACE(v,"SetOutputFilterParameters","destination=%p params=%p operationSet=%u -> 0x%x",a,b,c,hr);return hr;}
static void VA_API output_filter_get(VaVoice*v,void*a,void*b) {typedef void(VA_API*F)(void*,void*,void*);IN(v);if(LOST(v))filter_default(b);else FN(v,11,F)(R(v),va_unwrap_voice(a),b);OUT();VA_TRACE(v,"GetOutputFilterParameters","destination=%p out=%p -> void",a,b);}
/* 3.1 T2: MasterVolume m != 1: the real master gets game*m; shadow, trace and GetVolume keep the game value
   (noshadow: va_master_game_vol). */
static VaHR VA_API volume_set(VaVoice*v,float a,VaU32 b) {
    typedef VaHR(VA_API*F)(void*,float,VaU32);int mv=v->kind==2 && va_master_volume!=1.0f;float r=mv?a*va_master_volume:a;
    VaHR hr=CALL(v,"SetVolume",FN(v,12,F)(R(v),r,b));SH(va_sh_volume(v,a,b,hr));
    if(mv && !VA_SHADOWED(v) && !b && hr>=0)va_master_game_vol=a;
    VA_TRACE(v,"SetVolume","volume=%f operationSet=%u -> 0x%x",va_fbits(a),b,hr);return hr;
}
static void VA_API volume_get(VaVoice*v,float*a) {
    typedef void(VA_API*F)(void*,float*);
    if(v->kind==2 && va_master_volume!=1.0f){if(VA_SHADOWED(v))va_sh_get_float(v,0,a);else if(a)*a=va_master_game_vol;}
    else{IN(v);if(LOST(v))va_sh_get_float(v,0,a);else FN(v,13,F)(R(v),a);OUT();}
    VA_TRACE(v,"GetVolume","out=%p -> void",a);
}
static VaHR VA_API channels_set(VaVoice*v,VaU32 a,const float*b,VaU32 c) {typedef VaHR(VA_API*F)(void*,VaU32,const float*,VaU32);VaHR hr=CALL(v,"SetChannelVolumes",FN(v,14,F)(R(v),a,b,c));SH(va_sh_un(v,VA_UN_SetChannelVolumes,hr));VA_TRACE(v,"SetChannelVolumes","channels=%u volumes=%p operationSet=%u -> 0x%x",a,b,c,hr);return hr;}
static void VA_API channels_get(VaVoice*v,VaU32 a,float*b) {typedef void(VA_API*F)(void*,VaU32,float*);IN(v);if(LOST(v)){for(VaU32 i=0;b && i<a;++i)b[i]=1.0f;}else FN(v,15,F)(R(v),a,b);OUT();VA_TRACE(v,"GetChannelVolumes","channels=%u out=%p -> void",a,b);}
static VaHR VA_API matrix_set(VaVoice*v,void*a,VaU32 b,VaU32 c,const float*d,VaU32 e) {typedef VaHR(VA_API*F)(void*,void*,VaU32,VaU32,const float*,VaU32);VaHR hr=CALL(v,"SetOutputMatrix",FN(v,16,F)(R(v),va_unwrap_voice(a),b,c,d,e));SH(va_sh_matrix(v,a,b,c,d,e,hr));VA_TRACE(v,"SetOutputMatrix","destination=%p sourceChannels=%u destinationChannels=%u matrix=%p operationSet=%u -> 0x%x",a,b,c,d,e,hr);return hr;}
static void VA_API matrix_get(VaVoice*v,void*a,VaU32 b,VaU32 c,float*d) {typedef void(VA_API*F)(void*,void*,VaU32,VaU32,float*);IN(v);if(LOST(v))va_sh_get_matrix(v,a,b,c,d);else FN(v,17,F)(R(v),va_unwrap_voice(a),b,c,d);OUT();VA_TRACE(v,"GetOutputMatrix","destination=%p sourceChannels=%u destinationChannels=%u out=%p -> void",a,b,c,d);}
/* Registry node out under the lock; the real DestroyVoice (may wait on the XAudio2 thread) runs unlocked. */
/* Lost policy: va_sh_destroy unlinks the node and parks the real voice; no real DestroyVoice. */
static void VA_API destroy(VaVoice*v) {typedef void(VA_API*F)(void*);IN(v);
    if(VA_OUTAGE() && va_sh_destroy(v)){OUT();VA_TRACE(v,"DestroyVoice","-> void");HeapFree(GetProcessHeap(),0,v);return;}
    /* 1.4 T4 D7: the shim is detached under the lock, quiesced, then freed after the real DestroyVoice (real callbacks over). */
    int k=va_lock();VaShim*sh=0;if(k){sh=va_sh_detach(v);va_shadow_forget(v);}va_unlock(k);
    va_sh_quiesce(sh);va_sh_rb_wait(in_);FN(v,18,F)(R(v)); /* 1.5 T2: never under the rebuild's post-flip Start */va_sh_shim_free(sh);OUT();VA_TRACE(v,"DestroyVoice","-> void");HeapFree(GetProcessHeap(),0,v);}
static VaHR VA_API start(VaVoice*v,VaU32 a,VaU32 b) {typedef VaHR(VA_API*F)(void*,VaU32,VaU32);VaHR hr=CALL(v,"Start",FN(v,19,F)(R(v),a,b));SH(va_sh_start(v,a,b,hr));VA_TRACE(v,"Start","flags=0x%x operationSet=%u -> 0x%x",a,b,hr);return hr;}
static VaHR VA_API stop(VaVoice*v,VaU32 a,VaU32 b) {typedef VaHR(VA_API*F)(void*,VaU32,VaU32);VaHR hr=CALL(v,"Stop",FN(v,20,F)(R(v),a,b));SH(va_sh_stop(v,a,b,hr));VA_TRACE(v,"Stop","flags=0x%x operationSet=%u -> 0x%x",a,b,hr);return hr;}
static VaHR VA_API submit(VaVoice*v,const void*a,const void*b) {typedef VaHR(VA_API*F)(void*,const void*,const void*);VaHR hr;int live=1;IN(v);
    if(VA_OUTAGE()){if(va_sh_lost(v)){hr=0;live=0;}else{VaHR h=FN(v,21,F)(R(v),a,b);hr=va_sh_failed(v,"SubmitSourceBuffer",h);live=!(h<0 && hr>=0);}}
    else hr=FN(v,21,F)(R(v),a,b);
    SH(va_sh_submit(v,a,hr,live));OUT();VA_TRACE(v,"SubmitSourceBuffer","buffer=%p wma=%p -> 0x%x",a,b,hr);return hr;}
static VaHR VA_API flush(VaVoice*v) {typedef VaHR(VA_API*F)(void*);VaHR hr=CALL(v,"FlushSourceBuffers",FN(v,22,F)(R(v)));SH(va_sh_flush(v,hr));VA_TRACE(v,"FlushSourceBuffers","-> 0x%x",hr);return hr;}
static VaHR VA_API discontinuity(VaVoice*v) {typedef VaHR(VA_API*F)(void*);VaHR hr=CALL(v,"Discontinuity",FN(v,23,F)(R(v)));SH(va_sh_un(v,VA_UN_Discontinuity,hr));VA_TRACE(v,"Discontinuity","-> 0x%x",hr);return hr;}
static VaHR VA_API exit_loop(VaVoice*v,VaU32 a) {typedef VaHR(VA_API*F)(void*,VaU32);VaHR hr=CALL(v,"ExitLoop",FN(v,24,F)(R(v),a));SH(va_sh_un(v,VA_UN_ExitLoop,hr));VA_TRACE(v,"ExitLoop","operationSet=%u -> 0x%x",a,hr);return hr;}
/* 2.7 XAUDIO2_VOICE_STATE: context +0, BuffersQueued +4, SamplesPlayed (u64) +8. */
static void VA_API state(VaVoice*v,void*a) {typedef void(VA_API*F)(void*,void*,VaU32);va_report_pending_errors();
    IN(v);if(LOST(v))va_sh_get_state(v,a); /* Lost: shadow queue, head context, SamplesPlayed advanced by the outage clock (1.4 T3) */
    else{FN(v,25,F)(R(v),a,0);SH(va_sh_state(v,a));}
    OUT();
    VA_TRACE(v,"GetState","out=%p queued=%u played=%Q -> void",a,a?((const VaU32*)a)[1]:0UL,a?*(const unsigned long long*)((const VaU8*)a+8):0ULL);}
static VaHR VA_API frequency_set(VaVoice*v,float a,VaU32 b) {typedef VaHR(VA_API*F)(void*,float,VaU32);VaHR hr=CALL(v,"SetFrequencyRatio",FN(v,26,F)(R(v),a,b));SH(va_sh_ratio(v,a,b,hr));VA_TRACE(v,"SetFrequencyRatio","ratio=%f operationSet=%u -> 0x%x",va_fbits(a),b,hr);return hr;}
static void VA_API frequency_get(VaVoice*v,float*a) {typedef void(VA_API*F)(void*,float*);IN(v);if(LOST(v))va_sh_get_float(v,1,a);else FN(v,27,F)(R(v),a);OUT();VA_TRACE(v,"GetFrequencyRatio","out=%p -> void",a);}
static VaHR VA_API sample_rate(VaVoice*v,VaU32 a) {typedef VaHR(VA_API*F)(void*,VaU32);VaHR hr=CALL(v,"SetSourceSampleRate",FN(v,28,F)(R(v),a));SH(va_sh_un(v,VA_UN_SetSourceSampleRate,hr));VA_TRACE(v,"SetSourceSampleRate","rate=%u -> 0x%x",a,hr);return hr;}
static void *voice_vt[29]={
    (void*)details,(void*)sends,(void*)effect_chain,(void*)enable,(void*)disable,(void*)effect_state,(void*)effect_set,(void*)effect_get,
    (void*)filter_set,(void*)filter_get,(void*)output_filter_set,(void*)output_filter_get,(void*)volume_set,(void*)volume_get,
    (void*)channels_set,(void*)channels_get,(void*)matrix_set,(void*)matrix_get,(void*)destroy,
    (void*)start,(void*)stop,(void*)submit,(void*)flush,(void*)discontinuity,(void*)exit_loop,(void*)state,(void*)frequency_set,(void*)frequency_get,(void*)sample_rate
};
void *va_wrap_voice(void *real,unsigned kind) {
    VaVoice *v;
    v=HeapAlloc(GetProcessHeap(),0,sizeof *v);
    if(!v)return 0; /* caller destroys real once the lock is released */
    v->vt=voice_vt;v->real=real; /* real NULL: an outage silent wrapper (Lost) */v->kind=kind;v->rec=0;return v;
}
