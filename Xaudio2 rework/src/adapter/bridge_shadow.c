/* Voice state + buffer shadow (Phase 1.3 T4, phase-ends logs/T4.md D1-D9): one record per registry node holding the
 * creation record, the census setters' state and a copy of the queued XAUDIO2_BUFFERs, reconciled against the
 * game's GetState; per-engine unshadowed-call counters and the final-Release summary. Lock policy: bridge_shadow.h. */
#include "bridge_shadow.h"
#define VA_E_FAIL ((VaHR)0x80004005L)
#define END_OF_STREAM 0x40
/* frozen/frozen_t/frozen_at: outage stall check (1.4 T2), first observation (va_clock ticks) of the current SamplesPlayed.
   Outage clock (1.4 T3, g_cs): pos = frames into the head, base = SamplesPlayed at the head's start (Live tracking);
   eos_pending: an END_OF_STREAM buffer retired and the next has not started (SamplesPlayed restarts at its start),
   eos_total = SamplesPlayed at that buffer's end; Lost: mark = va_clock ticks consumed up to, carry = the remainder
   (units 1/(freq*65536) frame), clk_init = mark set.
   1.4 T4 (g_cs): shim = this voice's callback shim (0 none); head_started = the head's START was delivered (or forwarded
   Live); backlog = Lost frames consume() left while the shim's ring was nearly full, added on the next tick. */
struct VaShadowRec { VaShadowEng *eng; VaShadowVoice s; VaU32 frozen; unsigned long long frozen_t,frozen_at;
    VaU32 pos,eos_pending,clk_init; unsigned long long base,eos_total,mark,carry;
    VaShim *shim; VaU32 head_started; unsigned long long backlog;
    VaU32 silent;   /* 1.5 T1: real NULL after a rebuild attempt (or born so): Lost-time hooks run while Live */
    VaU32 hold;     /* 1.5 T2: a rebuild attempt is recreating this voice: its outage clock is frozen (lost_tick returns) */
    unsigned long long sp_base; }; /* 1.5 T3: SamplesPlayed at the flip, added to the new real's (cleared at its EOS restart) */
VA_IMP void *VA_API CreateThread(void*,VaU32,VaU32(VA_API*)(void*),void*,VaU32,VaU32*);
VA_IMP void VA_API Sleep(VaU32);
VA_IMP VaU32 VA_API GetCurrentThreadId(void);
#define VA_OUTAGE_TICK_MS 5 /* outage thread idle sleep (1.4 T4) */
/* Byte copy through volatile: no memcpy in a /nodefaultlib build. */
static void copy(void*d,const void*s,unsigned n){volatile VaU8*o=d;const VaU8*i=s;while(n--)*o++=*i++;}
static int same_bytes(const void*a,const void*b,unsigned n){const VaU8*x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
/* Shim ring (1.4 T4, g_cs): a LOOP of the same ctx at the tail coalesces; full -> dropped (consume keeps >= 72 free). */
static void ev_push(VaShim*sh,VaU32 m,void*ctx,VaU32 count){
    if(m==5 && sh->ev_n){VaShimEv*t=&sh->ev[(sh->ev_head+sh->ev_n-1)&255];if(t->method==5 && t->ctx==ctx){t->count+=count;return;}}
    if(sh->ev_n>=256)return;
    VaShimEv*x=&sh->ev[(sh->ev_head+sh->ev_n++)&255];x->method=m;x->ctx=ctx;x->count=count;
}
/* Frames a buffer plays (1.4 T3): PlayLength, else AudioBytes/nBlockAlign (MS ADPCM tag 2: blocks * wSamplesPerBlock)
   minus PlayBegin; LoopCount loops of LoopLength (0: PlayBegin+base-LoopBegin) on top; LoopCount 255 = never ends. */
#define FRAMES_INF 0xFFFFFFFFUL
static VaU32 play_frames(const VaShadowVoice*s,const VaBuffer29*b){ /* without loops */
    const VaShadowFormat*f=&s->format;VaU32 n;
    if(b->PlayLength)return b->PlayLength;
    n=f->align?b->AudioBytes/f->align:0;
    if(f->tag==2 && f->cb_size>=2)n*=(VaU32)f->extra[0]|(VaU32)f->extra[1]<<8;
    return n>b->PlayBegin?n-b->PlayBegin:0;
}
static VaU32 frames(const VaShadowVoice*s,const VaBuffer29*b){
    VaU32 n=play_frames(s,b);
    if(!b->LoopCount)return n;
    if(b->LoopCount==255)return FRAMES_INF;
    VaU32 end=b->PlayBegin+n,loop=b->LoopLength?b->LoopLength:end>b->LoopBegin?end-b->LoopBegin:0;
    unsigned long long t=(unsigned long long)n+(unsigned long long)b->LoopCount*loop;
    return t>=FRAMES_INF?FRAMES_INF-1:(VaU32)t;
}
/* Retire the head (Lost consumption): counters as reconcile, the queue shifts, pos restarts. */
static void retire_head(VaShadowRec*r){
    VaShadowVoice*s=&r->s;
    ++s->retired;++r->eng->retired;++s->retired_since;
    if(s->queue[0].Flags&END_OF_STREAM){s->retired_since=0;r->eos_pending=1;}
    for(VaU32 i=1;i<s->queued;++i)copy(&s->queue[i-1],&s->queue[i],sizeof s->queue[0]);
    --s->queued;r->pos=0;r->head_started=0;
}
/* Loop ends crossed in (old,now] (1.4 T4): end k (1..LoopCount, unbounded for 255) at first+(k-1)*loop. */
static VaU32 loops_crossed(const VaShadowVoice*s,const VaBuffer29*b,VaU32 old,VaU32 now){
    if(!b->LoopCount)return 0;
    VaU32 n=play_frames(s,b),end=b->PlayBegin+n,loop=b->LoopLength?b->LoopLength:end>b->LoopBegin?end-b->LoopBegin:0;
    if(!loop)return 0;
    VaU32 first=b->LoopBegin+loop>b->PlayBegin?b->LoopBegin+loop-b->PlayBegin:0;if(first>n)first=n;
    VaU32 c[2],x[2]={old,now};
    for(int i=0;i<2;++i){c[i]=x[i]<first?0:(x[i]-first)/loop+1;if(b->LoopCount!=255 && c[i]>b->LoopCount)c[i]=b->LoopCount;}
    return c[1]-c[0];
}
/* Lost: n frames through the queue as the mock's advance(); an empty queue drops the rest (SamplesPlayed frozen).
   1.4 T4, voice with a shim: START/LOOP/END/STREAMEND into its ring; ring free < 72 -> stop, returns the frames not taken. */
static unsigned long long consume(VaShadowRec*r,unsigned long long n){
    VaShadowVoice*s=&r->s;VaShim*sh=r->shim;
    while(n && s->queued){
        if(sh && 256-sh->ev_n<72)return n;
        VaU32 total=frames(s,&s->queue[0]),old=r->pos;void*ctx=s->queue[0].pContext;
        if(!r->pos && r->eos_pending){s->samples_played=0;r->eos_pending=0;}
        if(sh && !r->pos && !r->head_started){ev_push(sh,3,ctx,1);r->head_started=1;}
        if(total==FRAMES_INF){s->samples_played+=n;r->pos=r->pos+n>=FRAMES_INF?FRAMES_INF-1:r->pos+(VaU32)n;
            if(sh){VaU32 l=loops_crossed(s,&s->queue[0],old,r->pos);if(l)ev_push(sh,5,ctx,l);}
            return 0;}
        VaU32 take=total-r->pos;if(take>n)take=(VaU32)n;
        r->pos+=take;s->samples_played+=take;n-=take;
        if(sh){VaU32 l=loops_crossed(s,&s->queue[0],old,r->pos);if(l)ev_push(sh,5,ctx,l);}
        if(r->pos>=total){
            if(sh){ev_push(sh,4,ctx,1);if(s->queue[0].Flags&END_OF_STREAM)ev_push(sh,2,0,1);}
            retire_head(r);
        }
    }
    return 0;
}
/* Lost-time hooks call this first (g_cs): elapsed ticks since mark -> frames at rate*ratio (q16), whole seconds and
   remainder split so nothing overflows, remainder carried. Stopped: mark moves, nothing consumed. */
static void lost_tick(VaShadowRec*r){
    VaShadowEng*e=r->eng;VaShadowVoice*s=&r->s;
    if((!e->lost && !r->silent) || s->kind || r->hold)return;
    unsigned long long d,now=va_clock(&d);
    if(!r->clk_init){r->clk_init=1;r->mark=e->lost_at;}
    if(r->backlog){unsigned long long b=r->backlog;r->backlog=0;r->backlog=consume(r,b);} /* 1.4 T4: ring had drained */
    if(s->started && s->rate && d && now>r->mark){
        float x=s->ratio;if(s->max_ratio>0.0f && x>s->max_ratio)x=s->max_ratio;
        int q=x>0.0f?(x<65536.0f?(int)(x*65536.0f+0.5f):0x7FFFFFFF):0;
        unsigned long long fps=(unsigned long long)s->rate*(VaU32)q,fw=fps>>16,ff=fps&0xFFFF,t,r1;
        unsigned long long sec=udiv64(now-r->mark,d,&t),x2=sec*ff,n=sec*fw+(x2>>16),rem=(x2&0xFFFF)*d;
        n+=udiv64(t*fw,d,&r1);rem+=(r1<<16)+t*ff+r->carry;
        n+=udiv64(rem,d<<16,&r->carry);
        r->backlog+=consume(r,n);
    }
    r->mark=now;
}
#define VA_UN_NAME(m) #m,
static const char *const un_names[VA_UN_N]={VA_UN_LIST(VA_UN_NAME)};

VaShadowRec *va_sh_rec_new(VaShadowEng*e,unsigned kind){
    VaShadowRec*r=HeapAlloc(GetProcessHeap(),8,sizeof *r);if(!r)return 0;
    r->eng=e;r->s.kind=kind;r->s.volume=1.0f;r->s.ratio=1.0f;
    if(e->lost){r->clk_init=1;r->mark=va_clock(0);} /* born in Lost: its clock starts now */
    ++e->created;if(++e->live>e->peak)e->peak=e->live;
    return r;
}
void va_sh_silence(VaShadowRec*r){
    if(!r)return;
    r->silent=1;
    if(!r->clk_init){r->clk_init=1;r->mark=r->eng->lost?r->eng->lost_at:va_clock(0);}
}
VaU32 va_sh_rate(const VaShadowRec*r){return r?r->s.rate:0;}
void va_sh_hold(VaShadowRec*r){if(r && !r->hold){lost_tick(r);r->hold=1;}}
void va_sh_unhold(VaShadowRec*r){if(r)r->hold=0;}
const VaShadowVoice *va_sh_voice(const VaShadowRec*r){return &r->s;}
void *va_sh_callback(const VaShadowRec*r){return r->shim?(void*)r->shim:r->s.callback;}
/* 1.5 T3 rebuild resume: a voice keeps its queue across the flip when the game can see buffers end (callback) or loops. */
int va_sh_keeps(const VaShadowRec*r){
    const VaShadowVoice*s=&r->s;if(s->callback)return 1;
    for(VaU32 i=0;i<s->queued;++i)if(s->queue[i].LoopCount)return 1;
    return 0;
}
/* 1.5 T3 (g_cs): the head trimmed to its remaining frames from pos (PCM/float/extensible/ADPCM; ADPCM rounds down to a
   block); loop ends already crossed come off LoopCount, past the last one the loop is gone. */
void va_sh_resume(const VaShadowRec*r,VaBuffer29*out){
    const VaShadowVoice*s=&r->s;const VaBuffer29*b=&s->queue[0];VaU32 pos=r->pos,tag=s->format.tag;
    copy(out,b,sizeof *out);
    if(!pos || (tag!=1 && tag!=3 && tag!=0xFFFE && tag!=2))return;
    VaU32 n=play_frames(s,b),end=b->PlayBegin+n,L=b->LoopLength?b->LoopLength:end>b->LoopBegin?end-b->LoopBegin:0,p,loops;
    VaU32 first=b->LoopBegin+L>b->PlayBegin?b->LoopBegin+L-b->PlayBegin:0;if(first>n)first=n;
    if(!b->LoopCount || !L || pos<first){p=b->PlayBegin+pos;loops=b->LoopCount;}
    else{
        VaU32 c=(pos-first)/L+1;
        if(b->LoopCount!=255 && c>b->LoopCount){p=b->LoopBegin+L+(pos-first-b->LoopCount*L);loops=0;}
        else{p=b->LoopBegin+(pos-first)%L;loops=b->LoopCount==255?255:b->LoopCount-c;}
    }
    if(tag==2 && s->format.cb_size>=2){VaU32 spb=(VaU32)s->format.extra[0]|(VaU32)s->format.extra[1]<<8;if(spb)p-=p%spb;}
    out->PlayBegin=p;out->PlayLength=end-p;
    if(loops){out->LoopLength=L;out->LoopCount=loops;}
    else{out->LoopBegin=0;out->LoopLength=0;out->LoopCount=0;}
}
void va_sh_attach(VaShadowRec*r,VaU32 drop){
    VaShadowVoice*s=&r->s;VaU32 hs=r->head_started && !drop;
    r->sp_base=s->samples_played; /* 1.5 T3: the game's SamplesPlayed continues from here */
    if(drop){ /* 1.5 T3: not resubmitted (no callback, no loop): retired at the flip */
        if(drop>s->queued)drop=s->queued;
        s->retired+=drop;r->eng->retired+=drop;s->retired_since+=drop;
        for(VaU32 i=drop;i<s->queued;++i)copy(&s->queue[i-drop],&s->queue[i],sizeof s->queue[0]);
        s->queued-=drop;
    }
    else if(s->queued){VaBuffer29 h;va_sh_resume(r,&h);copy(&s->queue[0],&h,sizeof h);} /* 1.5 T3: as resubmitted, pos not reset yet */
    r->silent=0;r->hold=0;r->pos=0;r->base=0;r->eos_pending=0;r->frozen=0;r->clk_init=0;r->backlog=0;r->head_started=0;
    VaShim*sh=r->shim;if(!sh)return;
    sh->owed_n=0;sh->early_n=0;sh->stream_end_owed=0;
    for(VaU32 i=0;i<r->s.queued && i<128;++i){VaShimBuf*o=&sh->owed[sh->owed_n++];copy(&o->b,&r->s.queue[i],sizeof o->b);o->started=0;o->flushed=0;}
    if(sh->owed_n)sh->owed[0].started=hs?2:0; /* 1.5 T3: 2 = START delivered before the swap, the real one is not forwarded */
}
void va_sh_rec_free(VaShadowRec*r,int destroyed){
    if(!r)return;
    if(destroyed){++r->eng->destroyed;--r->eng->live;}
    HeapFree(GetProcessHeap(),0,r);
}
int va_sh_count(VaShadowEng*e,int method){return ++e->un[method]==1?method:-1;}
void va_sh_note(int method){
    if(method<0)return;
    char line[96];unsigned n=va_format(line,sizeof line,"shadow: unshadowed %s (first hit)\r\n",un_names[method]);
    if(n)va_log(line);
}
/* Sends as the game passed them (wrapper pointers), first 4 copied; returns 1 when more. */
static int record_sends(VaShadowVoice*s,const VaSends*in){
    s->sends=in;if(!in)return 0;
    s->send_count=in->SendCount;
    for(VaU32 i=0;i<in->SendCount && i<4 && in->pSends;++i){s->send[i].Flags=in->pSends[i].Flags;s->send[i].pOutputVoice=in->pSends[i].pOutputVoice;}
    return in->SendCount>4;
}
int va_sh_source(VaShadowRec*r,const void*fmt,VaU32 flags,float max_ratio,void*cb,const VaSends*sends,const void*effects){
    VaShadowVoice*s=&r->s;int over=record_sends(s,sends);
    s->flags=flags;s->max_ratio=max_ratio;s->callback=cb;s->effects=effects;
    if(fmt){
        const VaU8*f=fmt;VaShadowFormat*o=&s->format;
        copy(&o->tag,f,2);copy(&o->channels,f+2,2);copy(&o->rate,f+4,4);copy(&o->avg_bytes,f+8,4);
        copy(&o->align,f+12,2);copy(&o->bits,f+14,2);
        if(o->tag!=1){ /* a PCMWAVEFORMAT may be 16 bytes: +16 is read only for other tags */
            copy(&o->cb_size,f+16,2);
            if(o->cb_size>64)over=1;
            copy(o->extra,f+18,o->cb_size>64?64:o->cb_size);
        }
        s->channels=o->channels;s->rate=o->rate;
    }
    return over?va_sh_count(r->eng,VA_UN_CreateSourceVoice):-1;
}
int va_sh_submix(VaShadowRec*r,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 stage,const VaSends*sends,const void*effects){
    VaShadowVoice*s=&r->s;int over=record_sends(s,sends);
    s->channels=channels;s->rate=rate;s->flags=flags;s->stage=stage;s->effects=effects;
    return over?va_sh_count(r->eng,VA_UN_CreateSubmixVoice):-1;
}
void va_sh_master(VaShadowRec*r,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 index,const void*effects){
    VaShadowVoice*s=&r->s;s->channels=channels;s->rate=rate;s->flags=flags;s->device_index=index;s->effects=effects;
}
void va_sh_device(VaShadowEng*e,const void*device){copy(e->device,device,sizeof e->device);e->have_device=1;}

/* ---- Outage callbacks (Phase 1.4 T4, logs/T4.md D1-D9). The game callback is never called under g_cs. ---- */
typedef void(VA_API*ShimP)(void*);typedef void(VA_API*ShimU)(void*,VaU32);
typedef void(VA_API*ShimC)(void*,void*);typedef void(VA_API*ShimE)(void*,void*,VaHR);
#define GAME(sh,n) ((*(void***)(sh)->game)[n])
static int early_take(VaShim*sh,void*ctx){
    for(VaU32 i=0;i<sh->early_n;++i)if(sh->early[i]==ctx){
        for(VaU32 j=i+1;j<sh->early_n;++j)sh->early[j-1]=sh->early[j];
        --sh->early_n;return 1;
    }
    return 0;
}
static void owed_remove(VaShim*sh,VaU32 i){
    for(VaU32 j=i+1;j<sh->owed_n;++j)copy(&sh->owed[j-1],&sh->owed[j],sizeof sh->owed[0]);
    --sh->owed_n;
}
/* D3, g_cs, after a real Submit: an early end (forwarded before this hook) matches -> no owed entry, and in Lost no
   shadow append either (returns 1); else Live appends to owed (full: the oldest flushed goes, none -> untracked). */
static int shim_owe(VaShim*sh,const void*buffer,int is_live,int*u){
    const VaBuffer29*b=buffer;
    if(early_take(sh,b->pContext))return !is_live;
    if(!is_live)return 0;
    if(sh->owed_n>=128){
        VaU32 i=0;while(i<sh->owed_n && !sh->owed[i].flushed)++i;
        if(i==sh->owed_n){int m=va_sh_count(sh->eng,VA_UN_SubmitSourceBuffer);if(m>=0)*u=m;return 0;}
        owed_remove(sh,i);
    }
    VaShimBuf*o=&sh->owed[sh->owed_n++];copy(&o->b,b,sizeof o->b);o->started=0;o->flushed=0;
    return 0;
}
/* D3 Live Flush: a started voice keeps the first unflushed entry; every other unflushed one is flushed (2.9 ends it). */
static void shim_flush(VaShim*sh,VaU32 started){
    VaU32 i=0;while(i<sh->owed_n && sh->owed[i].flushed)++i;
    if(started && i<sh->owed_n)++i;
    for(;i<sh->owed_n;++i)sh->owed[i].flushed=1;
}
/* D2: real callbacks. 0/1/6 lock-free (lost -> dropped); 2-5 under g_cs (lost or dead -> dropped, else owed updated). */
static int shim_lost(VaShim*sh){return __atomic_load_n(&sh->eng->lost,__ATOMIC_SEQ_CST)!=0;}
static int shim_real(VaShim*sh,int m,void*ctx){
    int k=va_lock(),fwd=0;
    if(!sh->eng->lost && !__atomic_load_n(&sh->dead,__ATOMIC_SEQ_CST)){
        fwd=1;
        if(m==3){for(VaU32 i=0;i<sh->owed_n;++i)if(sh->owed[i].started!=1 && !sh->owed[i].flushed){
            if(sh->owed[i].started==2)fwd=0; /* 1.5 T3: resumed head, its START already delivered before the swap */
            sh->owed[i].started=1;break;}}
        else if(m==4){
            VaU32 i=0;while(i<sh->owed_n && sh->owed[i].b.pContext!=ctx)++i;
            if(i<sh->owed_n){if(sh->owed[i].b.Flags&END_OF_STREAM)sh->stream_end_owed=1;owed_remove(sh,i);}
            else{if(sh->early_n==8){for(VaU32 j=1;j<8;++j)sh->early[j-1]=sh->early[j];--sh->early_n;}sh->early[sh->early_n++]=ctx;}
            ++sh->real_ends;
        }
        else if(m==2)sh->stream_end_owed=0;
    }
    va_unlock(k);return fwd;
}
#define SHIM_IN(sh) __atomic_add_fetch(&(sh)->real_busy,1,__ATOMIC_SEQ_CST)
#define SHIM_OUT(sh) __atomic_sub_fetch(&(sh)->real_busy,1,__ATOMIC_SEQ_CST)
static void VA_API shim_pass_start(VaShim*sh,VaU32 b){SHIM_IN(sh);if(!shim_lost(sh))((ShimU)GAME(sh,0))(sh->game,b);SHIM_OUT(sh);}
static void VA_API shim_pass_end(VaShim*sh){SHIM_IN(sh);if(!shim_lost(sh))((ShimP)GAME(sh,1))(sh->game);SHIM_OUT(sh);}
static void VA_API shim_stream_end(VaShim*sh){SHIM_IN(sh);if(shim_real(sh,2,0))((ShimP)GAME(sh,2))(sh->game);SHIM_OUT(sh);}
static void VA_API shim_buffer_start(VaShim*sh,void*c){SHIM_IN(sh);if(shim_real(sh,3,c))((ShimC)GAME(sh,3))(sh->game,c);SHIM_OUT(sh);}
static void VA_API shim_buffer_end(VaShim*sh,void*c){SHIM_IN(sh);if(shim_real(sh,4,c))((ShimC)GAME(sh,4))(sh->game,c);SHIM_OUT(sh);}
static void VA_API shim_loop_end(VaShim*sh,void*c){SHIM_IN(sh);if(shim_real(sh,5,c))((ShimC)GAME(sh,5))(sh->game,c);SHIM_OUT(sh);}
static void VA_API shim_error(VaShim*sh,void*c,VaHR hr){SHIM_IN(sh);if(!shim_lost(sh))((ShimE)GAME(sh,6))(sh->game,c,hr);SHIM_OUT(sh);}
static void *shim_vt[]={(void*)shim_pass_start,(void*)shim_pass_end,(void*)shim_stream_end,(void*)shim_buffer_start,
                        (void*)shim_buffer_end,(void*)shim_loop_end,(void*)shim_error};
/* D4, g_cs, at Lost: owed ends become ring events; the shadow queue becomes the unflushed owed entries. */
static void realign(VaShadowEng*e){
    for(VaShim*sh=e->shims;sh;sh=sh->next){
        VaShadowRec*r=sh->rec;if(!r || sh->dead || r->silent)continue; /* 1.5 T1: silent recs keep their queue */
        VaShadowVoice*s=&r->s;VaU32 q=0,first=0;
        if(sh->stream_end_owed)ev_push(sh,2,0,1);
        for(VaU32 i=0;i<sh->owed_n;++i){
            if(sh->owed[i].flushed)ev_push(sh,4,sh->owed[i].b.pContext,1);
            else if(!q++)first=i;
        }
        if(q>64)q=64;
        int keep=s->queued && q && same_bytes(&s->queue[0],&sh->owed[first].b,sizeof s->queue[0]);
        long delta=(long)s->queued-(long)q,since=(long)s->retired_since+delta;
        for(VaU32 i=0,j=0;i<sh->owed_n && j<q;++i)if(!sh->owed[i].flushed)copy(&s->queue[j++],&sh->owed[i].b,sizeof s->queue[0]);
        s->queued=q;s->retired+=(VaU32)delta;e->retired+=(VaU32)delta;s->retired_since=since>0?(VaU32)since:0;
        if(!keep)r->pos=0;
        r->head_started=q?sh->owed[first].started:0;
        sh->owed_n=0;sh->stream_end_owed=0;
    }
}
/* D6: one per Lost outage engine; ticks every shim's clock, delivers one ring event at a time unlocked. */
static VaU32 VA_API outage_thread(void*p){
    VaShadowEng*e=p;
    while(!__atomic_load_n(&e->quit,__ATOMIC_SEQ_CST)){
        int k=va_lock();VaShim*pick=0;VaU32 m=0;void*ctx=0,*game=0;
        for(VaShim*sh=e->shims;sh;sh=sh->next)if(sh->rec && !sh->dead && sh->rec->s.kind==0)lost_tick(sh->rec);
        for(VaShim*sh=e->shims;sh && !pick;sh=sh->next)
            if(sh->ev_n && !sh->dead && sh->rec && (e->lost || sh->rec->silent) /* 1.5 T3: deliveries stop at the swap */
               && !__atomic_load_n(&sh->real_busy,__ATOMIC_SEQ_CST))pick=sh;
        if(pick){
            VaShimEv*x=&pick->ev[pick->ev_head];m=x->method;ctx=x->ctx;
            if(m==5 && x->count>1)--x->count;else{pick->ev_head=(pick->ev_head+1)&255;--pick->ev_n;}
            game=pick->game;__atomic_store_n(&pick->busy,1,__ATOMIC_SEQ_CST);
        }
        va_unlock(k);
        if(!pick){Sleep(VA_OUTAGE_TICK_MS);continue;}
        void**vt=*(void***)game;
        if(m==2)((ShimP)vt[2])(game);else((ShimC)vt[m])(game,ctx);
        __atomic_store_n(&pick->busy,0,__ATOMIC_RELEASE);
    }
    return 0;
}
VaShim *va_sh_shim_new(VaShadowEng*e,void*game){
    VaShim*sh=HeapAlloc(GetProcessHeap(),8,sizeof *sh);if(!sh)return 0;
    sh->vt=shim_vt;sh->game=game;sh->eng=e;return sh;
}
void va_sh_shim_link(VaShadowEng*e,VaShim*sh,VaShadowRec*r){
    sh->next=e->shims;e->shims=sh;
    if(r){r->shim=sh;sh->rec=r;}else __atomic_store_n(&sh->dead,1,__ATOMIC_SEQ_CST);
}
VaShim *va_sh_detach(VaVoice*v){
    VaShadowRec*r=v->rec;VaShim*sh=r?r->shim:0;
    if(!sh)return 0;
    __atomic_store_n(&sh->dead,1,__ATOMIC_SEQ_CST);sh->rec=0;r->shim=0;sh->ev_head=sh->ev_n=0;
    return sh;
}
void va_sh_quiesce(VaShim*sh){
    if(!sh || GetCurrentThreadId()==sh->eng->thread_id)return;
    for(VaU32 i=0;__atomic_load_n(&sh->busy,__ATOMIC_ACQUIRE);++i)Sleep(i<64?0:1);
}
void va_sh_shim_free(VaShim*sh){
    if(!sh)return;
    int k=va_lock();
    for(VaShim**p=&sh->eng->shims;*p;p=&(*p)->next)if(*p==sh){*p=sh->next;break;}
    va_unlock(k);HeapFree(GetProcessHeap(),0,sh);
}

/* Voice hooks: real call already made; hr<0 changes and counts nothing. */
#define ENTER(v) int u=-1,k=va_lock();VaShadowRec*r=k?(v)->rec:0
#define LEAVE() va_unlock(k);va_sh_note(u)
void va_sh_un(VaVoice*v,int method,VaHR hr){
    if(hr<0)return;
    ENTER(v);if(r)u=va_sh_count(r->eng,method);LEAVE();
}
void va_sh_volume(VaVoice*v,float volume,VaU32 opset,VaHR hr){
    if(hr<0)return;
    ENTER(v);if(r){if(opset)u=va_sh_count(r->eng,VA_UN_SetVolume);else r->s.volume=volume;}LEAVE();
}
void va_sh_ratio(VaVoice*v,float ratio,VaU32 opset,VaHR hr){
    if(hr<0)return;
    ENTER(v);if(r){lost_tick(r);if(opset)u=va_sh_count(r->eng,VA_UN_SetFrequencyRatio);else r->s.ratio=ratio;}LEAVE();
}
/* Up to 2 destinations keyed by the destination as passed; dst<=2 && src*dst<=16; same key replaces. */
void va_sh_matrix(VaVoice*v,void*dest,VaU32 src,VaU32 dst,const float*m,VaU32 opset,VaHR hr){
    if(hr<0)return;
    ENTER(v);
    if(r){
        VaShadowVoice*s=&r->s;VaShadowMatrix*x=0;
        if(!opset && m && dst<=2 && src<=16 && src*dst<=16){
            for(VaU32 i=0;i<s->matrices;++i)if(s->matrix[i].dest==dest)x=&s->matrix[i];
            if(!x && s->matrices<2){x=&s->matrix[s->matrices++];x->dest=dest;}
        }
        if(x){x->src=src;x->dst=dst;for(VaU32 i=0;i<16;++i)x->m[i]=i<src*dst?m[i]:0.0f;}
        else u=va_sh_count(r->eng,VA_UN_SetOutputMatrix);
    }
    LEAVE();
}
void va_sh_start(VaVoice*v,VaU32 flags,VaU32 opset,VaHR hr){
    if(hr<0)return;
    ENTER(v);if(r){lost_tick(r);if(opset)u=va_sh_count(r->eng,VA_UN_Start);else{r->s.started=1;r->s.start_flags=flags;r->s.retired_since=0;}}LEAVE();
}
void va_sh_stop(VaVoice*v,VaU32 flags,VaU32 opset,VaHR hr){
    if(hr<0)return;
    ENTER(v);if(r){lost_tick(r);if(opset)u=va_sh_count(r->eng,VA_UN_Stop);else{r->s.started=0;r->s.stop_flags=flags;}}LEAVE();
}
/* Order-only by count: while more than q are shadowed, retire the head. An END_OF_STREAM buffer zeroes retired_since.
   Live clock tracking (1.4 T3): a retire adds its frames to base; an EOS retire restarts base and arms eos_pending,
   which a later retire in the same pass clears (that buffer started from 0). */
static void reconcile(VaShadowRec*r,VaU32 q){
    VaShadowVoice*s=&r->s;
    if(s->queued<=q)return; /* fewer: a Submit racing on another thread; nothing */
    VaU32 d=s->queued-q;
    for(VaU32 i=0;i<d;++i){
        ++s->retired;++r->eng->retired;++s->retired_since;
        VaU32 f=frames(s,&s->queue[i]);r->eos_pending=0;
        if(s->queue[i].Flags&END_OF_STREAM){s->retired_since=0;r->eos_total=r->base+f;r->base=0;r->eos_pending=1;}
        else r->base+=f;
    }
    r->pos=0;
    for(VaU32 i=0;i<q;++i)copy(&s->queue[i],&s->queue[i+d],sizeof s->queue[i]);
    s->queued=q;
}
void va_sh_submit(VaVoice*v,const void*buffer,VaHR hr,int live){
    if(hr<0 || !buffer)return;
    typedef void(VA_API*F)(void*,VaVoiceState29*,VaU32);VaVoiceState29 st;
    st.pCurrentBufferContext=0;st.BuffersQueued=0;st.SamplesPlayed=0;
    if(live)((F)(*(void***)v->real)[25])(v->real,&st,0); /* unlocked (T3); Lost: no real call, no reconcile */
    ENTER(v);
    if(r){
        VaShadowVoice*s=&r->s;
        if(live)reconcile(r,st.BuffersQueued?st.BuffersQueued-1:0);
        lost_tick(r);
        int skip=r->shim && live && shim_owe(r->shim,buffer,!r->eng->lost,&u); /* 1.4 T4 D3 */
        if(!skip){
            if(s->queued<64)copy(&s->queue[s->queued++],buffer,sizeof s->queue[0]);
            else u=va_sh_count(r->eng,VA_UN_SubmitSourceBuffer);
        }
        ++s->submitted;++r->eng->submitted;
    }
    LEAVE();
}
/* 2.9 Flush (R1.3-005): started && queued -> the playing head stays, else all go; dropped count as retired.
   GetState right after a Flush is not trusted, so this does not reconcile. */
void va_sh_flush(VaVoice*v,VaHR hr){
    if(hr<0)return;
    ENTER(v);
    if(r){
        lost_tick(r);
        VaShadowVoice*s=&r->s;VaU32 keep=s->started && s->queued?1:0,drop=s->queued-keep;
        if(r->shim && (r->eng->lost || r->silent)){ /* 1.4 T4 D5: END per dropped buffer, no START/STREAMEND */
            for(VaU32 i=keep;i<s->queued;++i)ev_push(r->shim,4,s->queue[i].pContext,1);
            if(!keep){r->head_started=0;r->backlog=0;}
        }
        else if(r->shim)shim_flush(r->shim,s->started); /* D3 */
        s->queued=keep;s->retired+=drop;r->eng->retired+=drop;s->retired_since+=drop;
        if(!keep){r->pos=0;r->base=r->eos_pending?0:s->samples_played>r->sp_base?s->samples_played-r->sp_base:0;} /* stopped/empty: the next buffer starts fresh (1.5 T3: raw real units) */
    }
    LEAVE();
}
/* Outage stall (g_cs): started, real buffers queued and SamplesPlayed unchanged for >= 1000 ms since its first
   observation -> signal stall and take it here; reset when played changes or the condition fails. */
static int stall(VaShadowRec*r,const VaVoiceState29*st,char*line,unsigned cap){
    VaShadowEng*e=r->eng;
    if(e->lost)return 0;
    if(!r->s.started || !st->BuffersQueued){r->frozen=0;return 0;}
    unsigned long long f,now=va_clock(&f);
    if(!r->frozen || r->frozen_at!=st->SamplesPlayed){r->frozen=1;r->frozen_t=now;r->frozen_at=st->SamplesPlayed;return 0;}
    if(now-r->frozen_t<f)return 0; /* < 1 s of ticks */
    va_sh_signal(e,VA_LOST_STALL,0,0);return va_sh_take(e,line,cap);
}
void va_sh_state(VaVoice*v,void*out){
    if(!out)return;
    char line[128];int n=0;
    ENTER(v);
    if(r){VaVoiceState29*st=out;reconcile(r,st->BuffersQueued);
        unsigned long long p=st->SamplesPlayed; /* Live clock tracking (1.4 T3): pos into the head (1.5 T3: raw real) */
        if(r->eos_pending && p!=r->eos_total){r->eos_pending=0;r->base=0;r->sp_base=0;}
        if(r->eos_pending || !r->s.queued || p<r->base)r->pos=0;
        else{unsigned long long d=p-r->base;VaU32 t=frames(&r->s,&r->s.queue[0]);r->pos=d>t?t:(VaU32)d;}
        if(r->eng->outage)n=stall(r,st,line,sizeof line);
        r->s.samples_played=p+r->sp_base;st->SamplesPlayed+=r->sp_base;} /* 1.5 T3: continues across a rebuild */
    LEAVE();
    if(n)va_log(line);
}

/* ---- Outage (Phase 1.4 T2): per engine Live -> Lost, terminal (1.5 T1: unless VinceAudio.rebuild). A signal claims the pending slot once (atomics only,
   any thread, the critical callback included); a game thread takes it under g_cs and logs the line after va_unlock. ---- */
void va_sh_signal(VaShadowEng*e,long reason,const char*method,VaHR hr){
    long z=0;
    if(!e->outage || !__atomic_compare_exchange_n(&e->claim,&z,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
    e->method=method;e->hr=(VaU32)hr;__atomic_store_n(&e->reason,reason,__ATOMIC_RELEASE);
}
int va_sh_take(VaShadowEng*e,char*line,unsigned cap){
    if(!e->outage || e->lost)return 0;
    long reason=__atomic_load_n(&e->reason,__ATOMIC_ACQUIRE);
    if(!reason)return 0;
    __atomic_store_n(&e->lost,1,__ATOMIC_SEQ_CST);e->lost_at=va_clock(0); /* the outage clock's start for records already alive */
    realign(e); /* 1.4 T4 D4; then the outage thread, once (failure: no callbacks in Lost) */
    if(!e->thread)e->thread=CreateThread(0,0,outage_thread,e,0,&e->thread_id);
    char why[96];va_sh_reason(e,why,sizeof why);
    return (int)va_format(line,cap,"outage: Lost (%s)\r\n",why);
}
unsigned va_sh_reason(VaShadowEng*e,char*out,unsigned cap){
    switch(__atomic_load_n(&e->reason,__ATOMIC_ACQUIRE)){
    case VA_LOST_CRITICAL:return va_format(out,cap,"critical error 0x%x",e->hr);
    case VA_LOST_FAILED:return va_format(out,cap,"%s failed 0x%x",e->method,e->hr);
    case VA_LOST_STALL:return va_format(out,cap,"stall");
    case VA_LOST_DEFAULT:return va_format(out,cap,"default device changed"); /* 1.5 T4 notifier */
    default:return va_format(out,cap,"XAudio2Create unavailable");
    }
}
static VaShadowEng *eng_of(VaVoice*v,int k){return k && v->rec?v->rec->eng:0;}
int va_sh_lost(VaVoice*v){
    char line[128];int n=0,k=va_lock();VaShadowEng*e=eng_of(v,k);
    if(e){va_nt_poll(e);n=va_sh_take(e,line,sizeof line);} /* 1.5 T4: a default change can signal first */
    int lost=!v->real || (e && e->lost),rb=e && e->lost && e->rebuild;
    va_unlock(k);if(n)va_log(line);
    if(rb){va_rebuild(e,1);lost=!v->real || __atomic_load_n(&e->lost,__ATOMIC_SEQ_CST);} /* 1.5 T1: callers counted */
    return lost;
}
VaShadowEng *va_sh_in(VaVoice*v){
    VaShadowRec*r=__atomic_load_n(&v->rec,__ATOMIC_RELAXED);VaShadowEng*e=r?r->eng:0;
    if(!e || !e->outage)return 0;
    __atomic_add_fetch(&e->calls,1,__ATOMIC_SEQ_CST);return e;
}
void va_sh_out(VaShadowEng*e){if(e)__atomic_sub_fetch(&e->calls,1,__ATOMIC_SEQ_CST);}
void va_sh_rb_wait(VaShadowEng*e){if(e)while(__atomic_load_n(&e->rb_go,__ATOMIC_ACQUIRE))Sleep(0);}
VaHR va_sh_failed(VaVoice*v,const char*method,VaHR hr){
    if(!va_sh_fatal(hr))return hr;
    char line[128];int n=0,lost=0,k=va_lock();VaShadowEng*e=eng_of(v,k);
    if(e){va_sh_signal(e,VA_LOST_FAILED,method,hr);n=va_sh_take(e,line,sizeof line);lost=(int)e->lost;}
    va_unlock(k);if(n)va_log(line);return lost?0:hr;
}
/* Lost policy: no real DestroyVoice (it may wait on a dead engine); a real voice is parked for the final Release. */
int va_sh_destroy(VaVoice*v){
    if(!va_sh_lost(v))return 0;
    int k=va_lock();VaShadowEng*e=eng_of(v,k);
    VaShim*sh=k?va_sh_detach(v):0; /* 1.4 T4 D7: not freed, the parked real voice may still call it */
    if(k)va_shadow_forget(v);
    if(e && v->real){VaParked*p=HeapAlloc(GetProcessHeap(),0,sizeof *p);if(p){p->real=v->real;p->next=e->parked;e->parked=p;}}
    va_unlock(k);
    va_sh_quiesce(sh);
    if(!v->real)__atomic_sub_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);
    return 1;
}
void va_sh_get_details(VaVoice*v,VaDetails27*out){
    out->CreationFlags=0;out->InputChannels=2;out->InputSampleRate=48000;
    ENTER(v);
    if(r){
        out->CreationFlags=r->s.flags;
        if(r->s.kind==2)out->InputSampleRate=r->eng->master_rate; /* masters are 2-channel (bridge_engine.c master) */
        else{out->InputChannels=r->s.channels;out->InputSampleRate=r->s.rate;}
    }
    LEAVE();
}
void va_sh_get_float(VaVoice*v,int ratio,float*out){
    if(!out)return;
    *out=1.0f;ENTER(v);if(r)*out=ratio?r->s.ratio:r->s.volume;LEAVE();
}
void va_sh_get_matrix(VaVoice*v,void*dest,VaU32 src,VaU32 dst,float*out){
    if(!out)return;
    for(VaU32 i=0;i<src*dst;++i)out[i]=0.0f;
    ENTER(v);
    if(r)for(VaU32 i=0;i<r->s.matrices;++i){
        const VaShadowMatrix*x=&r->s.matrix[i];
        if(x->dest==dest && x->src==src && x->dst==dst)for(VaU32 j=0;j<src*dst;++j)out[j]=x->m[j];
    }
    LEAVE();
}
void va_sh_get_state(VaVoice*v,void*out){
    if(!out)return;
    VaVoiceState29*st=out;st->pCurrentBufferContext=0;st->BuffersQueued=0;st->SamplesPlayed=0;
    ENTER(v);
    if(r){lost_tick(r);st->BuffersQueued=r->s.queued;st->pCurrentBufferContext=r->s.queued?r->s.queue[0].pContext:0;st->SamplesPlayed=r->s.samples_played;}
    LEAVE();
}

VaHR VA_API VinceAudioShadowVoice(const void*wrapper,VaShadowVoice*out){
    if(!out)return VA_E_INVALIDARG;
    int k=va_lock();VaShadowRec*r=k?va_shadow_find(wrapper):0;
    if(r){
        copy(out,&r->s,sizeof *out);
        if(r->s.kind==2 && r->eng->have_device){out->have_device=1;copy(out->device,r->eng->device,sizeof out->device);}
    }
    va_unlock(k);return r?0:VA_E_FAIL;
}
/* Built under g_cs at the final Release, logged by the caller after va_unlock. */
void va_sh_summary(VaShadowEng*e,char*line,unsigned cap){
    unsigned n=va_format(line,cap,"shadow: summary voices created %u destroyed %u peak live %u buffers submitted %u retired %u unshadowed",
                         e->created,e->destroyed,e->peak,e->submitted,e->retired);
    int any=0;
    for(int i=0;i<VA_UN_N;++i)if(e->un[i]){n+=va_format(line+n,cap-n," %s=%u",un_names[i],e->un[i]);any=1;}
    if(!any)n+=va_format(line+n,cap-n," none");
    va_format(line+n,cap-n,"\r\n");
}
