/* Private: voice state + buffer shadow (Phase 1.3 T4, logs/T4.md D1-D9), shared by bridge_voice.c, bridge_engine.c and
 * bridge_shadow.c. Lock policy (T3): every va_sh_* hook runs after the real call, takes g_cs itself (or expects it held
 * where noted), never calls XAudio2 under it and never logs under it (first-hit lines go through va_sh_note unlocked). */
#ifndef VA_BRIDGE_SHADOW_H
#define VA_BRIDGE_SHADOW_H
#include "bridge_voice.h"
typedef struct VaShadowRec VaShadowRec;
typedef struct { void **vt; void *real; unsigned kind; VaShadowRec *rec; } VaVoice; /* rec: g_cs; game uses vt only */
_Static_assert(sizeof(VaVoice)==16,"wrapper layout vt+0 real+4 kind+8 rec+12");
typedef struct { void *pCurrentBufferContext; VaU32 BuffersQueued; unsigned long long SamplesPlayed; } VaVoiceState29;
_Static_assert(sizeof(VaVoiceState29)==16,"XAUDIO2_VOICE_STATE x86");
/* Unshadowed methods (D7), counted per engine, summary order. */
#define VA_UN_LIST(X) X(SetOutputVoices) X(SetEffectChain) X(EnableEffect) X(DisableEffect) X(SetEffectParameters) \
    X(SetFilterParameters) X(SetOutputFilterParameters) X(SetVolume) X(SetChannelVolumes) X(SetOutputMatrix) X(Start) X(Stop) \
    X(SubmitSourceBuffer) X(Discontinuity) X(ExitLoop) X(SetFrequencyRatio) X(SetSourceSampleRate) X(CreateSourceVoice) \
    X(CreateSubmixVoice) X(RegisterForCallbacks) X(UnregisterForCallbacks) X(StartEngine) X(StopEngine) X(CommitChanges)
#define VA_UN_ENUM(m) VA_UN_##m,
enum { VA_UN_LIST(VA_UN_ENUM) VA_UN_N };
typedef struct VaParked { struct VaParked *next; void *real; } VaParked;
/* Outage callbacks (1.4 T4, logs/T4.md D1-D9): one shim (adapter IXAudio2VoiceCallback, vt first) per outage source
   voice with a callback. Live: forwards real callbacks, owed = submitted buffers whose OnBufferEnd is not forwarded yet;
   Lost: real callbacks dropped, the outage clock fills ev (FIFO), the engine's outage thread delivers it unlocked.
   g_cs: rec, next, owed*, early*, stream_end_owed, real_ends, ev*; atomics: real_busy, busy, dead. */
typedef struct { VaBuffer29 b; VaU32 started,flushed; } VaShimBuf;
typedef struct { VaU32 method; void *ctx; VaU32 count; } VaShimEv; /* method = vtable slot 2 StreamEnd 3 Start 4 End 5 Loop */
typedef struct VaShim {
    void **vt; void *game; struct VaShadowEng *eng; VaShadowRec *rec; struct VaShim *next;
    volatile long real_busy,busy,dead; /* real_busy: real callbacks in the shim; busy: the outage thread delivering */
    VaU32 owed_n; VaShimBuf owed[128]; void *early[8]; VaU32 early_n,stream_end_owed,real_ends;
    VaU32 ev_head,ev_n; VaShimEv ev[256];
} VaShim;
/* Per shadow engine (embedded in VaEngine, zeroed at create); g_cs. */
typedef struct VaShadowEng {
    VaU32 created,destroyed,live,peak,submitted,retired;
    VaU32 un[VA_UN_N];
    VaU32 have_device; VaU8 device[0x42c]; /* last successful GetDeviceDetails(0) */
    /* Outage (Phase 1.4 T2): outage = VinceAudio.outage at create (constant); lost (g_cs) is terminal unless rebuild.
       claim/reason/method/hr: the one pending signal, set with atomics only from any thread (va_sh_signal). */
    VaU32 outage,lost,master_rate; volatile long claim,reason; const char *method; VaU32 hr;
    VaParked *parked;             /* Lost DestroyVoice: real voices left to the final Release (g_cs) */
    unsigned long long lost_at;   /* va_clock() when va_sh_take went Lost (1.4 T3, g_cs) */
    VaShim *shims;                /* 1.4 T4: every shim of this engine (g_cs), freed at the final Release */
    void *thread; VaU32 thread_id; volatile long quit; /* outage thread, started once by va_sh_take */
    /* Rebuild (1.5 T1, VinceAudio.rebuild, implies outage): Lost -> attempts on game threads, one claimant (rb_busy CAS
       0->1); rb_* read/written only by the claimant. calls: wrapper paths using a real outside g_cs (seq_cst).
       1.5 T2: an attempt recreates the whole voice graph; rb_gone (g_cs): new reals of nodes destroyed during the attempt
       (va_shadow_forget), destroyed by the claimant; rb_job (g_cs): the node whose job runs unlocked (forget clears it).
       1.6 T3: rb_busy 0 free, 1 game claimant (an attempt's start or collect half), 2 closing (the final Release, CAS 0->2
       or 4->2, then waits calls <= 0; no attempt after), 2.2 T2: 3 the worker owns the job (CAS 1->3), 4 result published
       (CAS 3->4; a game entry collects it with 4->1).
       2.2 T2 ownership: the new engine and master belong to the worker until it publishes them by one atomic state change
       (rb_busy 3->4); the game thread touches them only after that change; release either takes them (and releases them)
       or, on its 5000 ms bound, leaks the VaEngine (so a late worker never writes into freed memory). XAudio2 is not
       documented as free-threaded (R2.2-004): no engine is ever used by two threads at once.
       wk_*: the job's inputs (set by the game before 1->3) and outputs (set by the job before 3->4); wk_mn (g_cs): the
       attempt's master node, cleared by va_shadow_forget when that node goes. syncrebuild: VinceAudio.syncrebuild at create
       (the job runs inline on the game thread, 2.1 behaviour). */
    VaU32 rebuild; volatile long rb_busy,calls; VaU32 rb_n,rb_delay_ms,rb_last_hr,rb_started; unsigned long long rb_next;
    VaU32 syncrebuild; void *wk_thread,*wk_wake; volatile long wk_quit; void *wk_mn;
    void *wk_old,*wk_mreal,*wk_create; VaParked *wk_parked,*wk_voices; VaU32 wk_has_master,wk_rate,wk_flags,wk_proc;
    void *wk_real,*wk_m; VaHR wk_hr; const char *wk_step; VaU32 wk_live_rate,wk_live_ch; unsigned long long wk_t[6],wk_pub;
    VaU32 wk_n,wk_last; unsigned long long wk_now,wk_f,wk_gpre;
    VaParked *rb_gone; void *rb_job;
    volatile long rb_go; /* 1.5 T2: the claimant's post-flip Start loop runs; a Live DestroyVoice waits it out (va_sh_rb_wait) */
    /* 1.5 T4 default-device notifier (bridge_engine.c): nt_seen = the notifier generation this engine acted on (CAS);
       nt_appear_seen = the appear count the claimant last consumed (rb_*-style, claimant only). */
    volatile long nt_seen; VaU32 nt_appear_seen;
    /* 1.6 T2: the last Live (create or rebuild): live_seq = its g_nt_live_seq, live_t = va_clock then (atomics) */
    volatile long live_seq; unsigned long long live_t;
} VaShadowEng;
/* Outage clock (1.4 T3), bridge_engine.c: shipped = QPC ticks (freq cached); VA_MOCK_BACKEND = the mock's MockClock
   (0 ticks, freq 1 when absent). Lock-free, callable under g_cs. freq may be NULL. */
unsigned long long va_clock(unsigned long long *freq);
unsigned long long udiv64(unsigned long long n,unsigned long long d,unsigned long long *rem); /* no __aulldiv */
/* Outage engines + live silent wrappers (real NULL); 0 = switch off everywhere: one relaxed load, no lock. */
extern volatile long va_outage_refs;
#define VA_OUTAGE() __builtin_expect(__atomic_load_n(&va_outage_refs,__ATOMIC_RELAXED),0)
/* 3.1 T1/T2, bridge_engine.c: MasterVolume m (VinceAudio.ini, 0.5 default); the noshadow game master volume (m != 1). */
extern float va_master_volume;
extern float va_master_game_vol;
enum { VA_LOST_CRITICAL=1, VA_LOST_FAILED, VA_LOST_STALL, VA_LOST_UNAVAILABLE, VA_LOST_DEFAULT };
/* A real-call HRESULT that signals Lost: failed, except E_INVALIDARG and XAUDIO2_E_INVALID_CALL (caller errors). */
static inline int va_sh_fatal(VaHR hr){return hr<0 && hr!=VA_E_INVALIDARG && hr!=(VaHR)0x88960001L;}
void va_sh_signal(VaShadowEng *e,long reason,const char *method,VaHR hr); /* atomics only, first signal wins */
int va_sh_take(VaShadowEng *e,char *line,unsigned cap); /* caller holds g_cs: pending -> lost once, returns line length */
unsigned va_sh_reason(VaShadowEng *e,char *out,unsigned cap); /* the pending signal as the Lost line's parens text */
/* Rebuild (1.5 T1), bridge_engine.c, unlocked: a Lost rebuild engine tries one throttled attempt (self = the caller's own
   count in calls, -1 = never attempt). va_sh_in/va_sh_out (bridge_shadow.c): count a wrapper path on an outage engine. */
void va_rebuild(VaShadowEng *e,int self);
/* 1.5 T4, bridge_engine.c, lock-free (callable under g_cs): a Live rebuild engine whose notifier generation moved and has
   been quiet for 100 ms signals VA_LOST_DEFAULT once per generation (atomics + va_clock only). */
void va_nt_poll(VaShadowEng *e);
VaShadowEng *va_sh_in(VaVoice *v);
void va_sh_out(VaShadowEng *e);
void va_sh_rb_wait(VaShadowEng *e); /* 1.5 T2, unlocked: until the post-flip Start loop is over (NULL ok) */
#define VA_SHADOWED(v) __atomic_load_n(&(v)->rec,__ATOMIC_RELAXED) /* unlocked hint, re-checked under g_cs */
/* Caller holds g_cs. */
VaShadowRec *va_sh_rec_new(VaShadowEng *e,unsigned kind);
void va_sh_rec_free(VaShadowRec *r,int destroyed);
void va_sh_silence(VaShadowRec *r); /* 1.5 T1: silent record (real NULL), its outage clock runs while Live */
VaU32 va_sh_rate(const VaShadowRec *r); /* creation rate as passed */
/* 1.5 T2 rebuild: hold = the outage clock ticked once, then frozen until unhold/attach; va_sh_voice = the shadow (read
   only); va_sh_callback = the shim, else the game's callback; va_sh_attach = the flip to a new real: Live tracking reset,
   silent/hold 0, the shim owes the shadow queue (early and stream-end cleared).
   1.5 T3: va_sh_attach(r,drop): sp_base = the shadow SamplesPlayed (added to the new real's); drop > 0 = the first drop
   entries retired (not resubmitted), else the head becomes va_sh_resume (as resubmitted) and owed[0].started = 2 when its
   START was delivered; va_sh_keeps = callback or a looping buffer (the queue is resubmitted); va_sh_resume = the head
   trimmed to its remaining frames from the outage clock's pos (loops crossed taken off). */
void va_sh_hold(VaShadowRec *r);
void va_sh_unhold(VaShadowRec *r);
const VaShadowVoice *va_sh_voice(const VaShadowRec *r);
void *va_sh_callback(const VaShadowRec *r);
int va_sh_keeps(const VaShadowRec *r);
void va_sh_resume(const VaShadowRec *r,VaBuffer29 *out);
void va_sh_attach(VaShadowRec *r,VaU32 drop);
int va_sh_source(VaShadowRec *r,const void *fmt,VaU32 flags,float max_ratio,void *cb,const VaSends *sends,const void *effects);
int va_sh_submix(VaShadowRec *r,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 stage,const VaSends *sends,const void *effects);
void va_sh_master(VaShadowRec *r,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 index,const void *effects);
int va_sh_count(VaShadowEng *e,int method); /* returns method on its first hit, else -1 */
void va_sh_device(VaShadowEng *e,const void *device);
/* 1.4 T4 shims: va_sh_shim_new unlocked (0 on OOM); va_sh_shim_link under g_cs (r NULL: adopt failed, linked dead);
   va_sh_detach under g_cs (dead, unlinked from its record, ring emptied; 0 = no shim); va_sh_quiesce unlocked (waits
   for an outage delivery in flight, not from the outage thread); va_sh_shim_free unlocked (unlinks + frees, NULL ok). */
VaShim *va_sh_shim_new(VaShadowEng *e,void *game);
void va_sh_shim_link(VaShadowEng *e,VaShim *sh,VaShadowRec *r);
VaShim *va_sh_detach(VaVoice *v);
void va_sh_quiesce(VaShim *sh);
void va_sh_shim_free(VaShim *sh);
void va_sh_summary(VaShadowEng *e,char *line,unsigned cap);
VaShadowRec *va_shadow_find(const void *wrapper); /* bridge_engine.c: registry lookup */
VaHR VA_API VinceAudioShadowVoice(const void *wrapper,VaShadowVoice *out); /* exported (D6) */
/* Unlocked. */
void va_sh_note(int method); /* the first-hit log line, method<0 = nothing */
void va_sh_un(VaVoice *v,int method,VaHR hr);
void va_sh_volume(VaVoice *v,float volume,VaU32 opset,VaHR hr);
void va_sh_ratio(VaVoice *v,float ratio,VaU32 opset,VaHR hr);
void va_sh_matrix(VaVoice *v,void *dest,VaU32 src,VaU32 dst,const float *m,VaU32 opset,VaHR hr);
void va_sh_start(VaVoice *v,VaU32 flags,VaU32 opset,VaHR hr);
void va_sh_stop(VaVoice *v,VaU32 flags,VaU32 opset,VaHR hr);
void va_sh_submit(VaVoice *v,const void *buffer,VaHR hr,int live); /* live: real GetState(flags 0) unlocked, then reconcile + append; else append */
void va_sh_flush(VaVoice *v,VaHR hr);
void va_sh_state(VaVoice *v,void *out); /* outage engine: stall check; 1.5 T3: out SamplesPlayed += sp_base */
/* Outage, unlocked: va_sh_lost takes a pending signal (logs it), 1 = Lost or silent wrapper (no real call);
   va_sh_failed after a real call: a fatal hr on an outage engine signals Lost and becomes S_OK. */
int va_sh_lost(VaVoice *v);
VaHR va_sh_failed(VaVoice *v,const char *method,VaHR hr);
int va_sh_destroy(VaVoice *v); /* Lost DestroyVoice: node out, real parked; 1 = done, caller frees the wrapper */
/* Lost getters: answered from the shadow. */
void va_sh_get_details(VaVoice *v,VaDetails27 *out);
void va_sh_get_float(VaVoice *v,int ratio,float *out);
void va_sh_get_matrix(VaVoice *v,void *dest,VaU32 src,VaU32 dst,float *out);
void va_sh_get_state(VaVoice *v,void *out);
/* bridge_engine.c */
void va_log(const char *line);
unsigned va_format(char *out,unsigned cap,const char *fmt,...); /* va_trace's %-formats, no prefix, NUL-terminated */
#endif
