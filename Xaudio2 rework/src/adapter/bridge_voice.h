#ifndef VA_BRIDGE_VOICE_H
#define VA_BRIDGE_VOICE_H
typedef unsigned long VaU32;
typedef long VaHR;
typedef unsigned short VaU16;
typedef unsigned char VaU8;
#define VA_API __stdcall
#define VA_IMP __declspec(dllimport)
#define VA_E_INVALIDARG ((VaHR)0x80070057L)
#define VA_E_OUTOFMEMORY ((VaHR)0x8007000EL)
#define VA_E_NOTIMPL ((VaHR)0x80004001L)
typedef struct { VaU32 Flags; void *pOutputVoice; } VaSend;
typedef struct { VaU32 SendCount; VaSend *pSends; } VaSends;
typedef struct { VaU32 CreationFlags, InputChannels, InputSampleRate; } VaDetails27;
typedef struct { VaU32 CreationFlags, ActiveFlags, InputChannels, InputSampleRate; } VaDetails29;
_Static_assert(sizeof(VaDetails27)==12,"legacy details ABI");
_Static_assert(sizeof(VaDetails29)==16,"modern details ABI");
_Static_assert(sizeof(VaSends)==8,"x86 sends ABI");
VA_IMP void *VA_API GetProcessHeap(void);
VA_IMP void *VA_API HeapAlloc(void *,VaU32,unsigned int);
VA_IMP int VA_API HeapFree(void *,VaU32,void *);
void *va_wrap_voice(void *real,unsigned kind); /* 0 on OOM; the caller destroys real (lock released); real NULL = outage silent wrapper */
void *va_unwrap_voice(void *legacy);
void va_report_pending_errors(void);
VaHR va_translate_sends(const VaSends *in,VaSends *out,VaSend *storage,unsigned capacity);
/* Shadow registry (VinceAudio.shadow beside the DLL): one process-wide recursive lock, taken only while a shadow
   engine is alive; va_lock returns whether it entered, va_unlock(that). va_shadow_forget unlinks+frees the
   registry node of a wrapper (caller holds the lock); absent = plain voice. */
int va_lock(void);
void va_unlock(int taken);
void va_shadow_forget(void *wrapper);
/* 2.9 XAUDIO2_BUFFER (x86), passed through raw. */
typedef struct { VaU32 Flags,AudioBytes; const VaU8 *pAudioData; VaU32 PlayBegin,PlayLength,LoopBegin,LoopLength,LoopCount; void *pContext; } VaBuffer29;
_Static_assert(sizeof(VaBuffer29)==36,"XAUDIO2_BUFFER x86");
/* Voice shadow snapshot (Phase 1.3 T4): VinceAudioShadowVoice(wrapper,out) copies it under the registry lock;
   out NULL -> E_INVALIDARG, wrapper not shadowed -> E_FAIL. Pointers are as the game passed them (wrappers). */
typedef struct { VaU16 tag,channels; VaU32 rate,avg_bytes; VaU16 align,bits,cb_size; VaU8 extra[64]; } VaShadowFormat; /* tag 1: cb_size 0, no extra */
typedef struct { void *dest; VaU32 src,dst; float m[16]; } VaShadowMatrix;
typedef struct {
    VaU32 kind;                          /* 0 source, 1 submix, 2 master */
    VaShadowFormat format;               /* source creation format */
    VaU32 flags,channels,rate,stage,device_index;
    float max_ratio; void *callback; const void *effects;
    const VaSends *sends; VaU32 send_count; VaSend send[4];
    float volume,ratio;                  /* OperationSet 0 setters; 1.0 initially */
    VaU32 matrices; VaShadowMatrix matrix[2];
    VaU32 started,start_flags,stop_flags;
    VaU32 queued; VaBuffer29 queue[64];  /* play order, head first; audio not copied */
    unsigned long long samples_played;   /* last GetState */
    VaU32 submitted,retired,retired_since; /* retired_since: since the last Start or END_OF_STREAM buffer */
    VaU32 have_device; VaU8 device[0x42c]; /* master: its engine's last GetDeviceDetails(0) */
} VaShadowVoice;
/* Call trace (VinceAudio.trace beside the DLL): one `T ` line per call into VinceAudio.log.
   fmt: %x u32 as 8 hex digits, %p pointer, %u u32 decimal, %Q u64 decimal, %f float bits (va_fbits),
   %m u64 QPC ticks as ms, %s string. Off = one flag load, arguments not evaluated. */
extern int va_trace_on;
void va_trace(const void *obj,const char *method,const char *fmt,...);
#define VA_TRACE(...) do{if(va_trace_on)va_trace(__VA_ARGS__);}while(0)
static inline VaU32 va_fbits(float f){union{float f;VaU32 u;}x;x.f=f;return x.u;}
#endif
