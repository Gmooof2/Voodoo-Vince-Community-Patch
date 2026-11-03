/* Game-specific XAudio2 2.7 interface backed by the Windows 10/11 2.9 engine.
 * Experimental scope: stereo/default output and automatic endpoint migration.
 * Critical invalidation is reported; with VinceAudio.rebuild a Lost engine, its
 * master (1.5 T1) and its whole voice graph with the shadowed state (1.5 T2) are
 * recreated. No system DLLs or device settings are changed.
 */
#include "bridge_shadow.h"
#include "version.h"

typedef struct { VaU32 a; VaU16 b,c; VaU8 d[8]; } VaGuid;
typedef struct {
    VaU16 device_id[256],display_name[256];
    VaU32 role;
    VaU16 format_tag,channels;
    VaU32 rate,bytes_per_second;
    VaU16 block_align,bits,extra_size,valid_bits;
    VaU32 channel_mask;
    VaGuid subformat;
} VaDevice;
_Static_assert(sizeof(VaDevice)==0x42c,"legacy device details size");
_Static_assert(__builtin_offsetof(VaDevice,channel_mask)==0x418,"legacy channel mask");

/* rb_*: 1.5 T2 rebuild attempt (g_cs, claimant only): rb_real = the new real (not for the master), rb_skip = kept silent,
   rb_round = last reconcile round visited, rb_done + the snapshot of what was applied (volume, ratio, matrices; queued,
   submitted, retired at the apply). 1.5 T3: rb_drop = shadow entries not resubmitted at creation (retired at the flip). */
typedef struct VaShadowNode { struct VaShadowNode *next; void *engine; void *wrapper; unsigned kind; VaShadowRec *rec;
    void *rb_real; VaU32 rb_skip,rb_round,rb_done,rb_q,rb_sub,rb_ret,rb_mats; float rb_vol,rb_ratio; VaShadowMatrix rb_mat[2];
    VaU32 rb_drop; } VaShadowNode;
typedef struct VaEngine {
    void **vt;
    volatile long refs;
    void *real;
    VaU32 master_rate;
    void **callback_vt;
    int shadow;                    /* VinceAudio.shadow at create: registry on */
    VaShadowNode *nodes;           /* this engine's wrappers, creation order */
    struct VaEngine *next_shadow;  /* global shadow-engine chain */
    VaShadowEng sh;                /* shadow counters + device copy (T4), g_cs */
    VaHR (VA_API *create)(void**,VaU32,VaU32); /* 1.5 T1: XAudio2Create, cached at Initialize (rebuild reloads if NULL) */
    VaU32 init_flags,init_processor,have_init; /* 1.5 T1: Initialize arguments for a rebuild */
} VaEngine;

#define VA_E_FAIL ((VaHR)0x80004005L)
#define VA_E_NOINTERFACE ((VaHR)0x80004002L)
#define VA_E_NOTIMPL ((VaHR)0x80004001L)
#define REAL(e,n,T) ((T)(*(void***)(e)->real)[n])
VA_IMP void *VA_API LoadLibraryW(const VaU16*);
VA_IMP VaU32 VA_API GetSystemDirectoryW(VaU16*,VaU32);
VA_IMP void *VA_API GetProcAddress(void*,const char*);
VA_IMP VaU32 VA_API GetModuleFileNameW(void*,VaU16*,VaU32);
VA_IMP void *VA_API CreateFileW(const VaU16*,VaU32,VaU32,void*,VaU32,VaU32,void*);
VA_IMP int VA_API WriteFile(void*,const void*,VaU32,VaU32*,void*);
VA_IMP int VA_API ReadFile(void*,void*,VaU32,VaU32*,void*); /* 3.1 T1: VinceAudio.ini */
VA_IMP int VA_API CloseHandle(void*);
VA_IMP VaU32 VA_API GetFileAttributesW(const VaU16*);
VA_IMP int VA_API QueryPerformanceCounter(long long*);
VA_IMP int VA_API QueryPerformanceFrequency(long long*);
VA_IMP VaU32 VA_API GetCurrentThreadId(void);
typedef struct { void *opaque[6]; } VaCriticalSection; /* x86 CRITICAL_SECTION, no SDK headers */
_Static_assert(sizeof(VaCriticalSection)==24,"x86 CRITICAL_SECTION");
VA_IMP void VA_API InitializeCriticalSection(VaCriticalSection*);
VA_IMP void VA_API EnterCriticalSection(VaCriticalSection*);
VA_IMP void VA_API LeaveCriticalSection(VaCriticalSection*);
VA_IMP void VA_API Sleep(VaU32);
VA_IMP VaU32 VA_API WaitForSingleObject(void*,VaU32); /* 1.4 T4: outage thread stop */
VA_IMP VaU32 VA_API WaitForMultipleObjects(VaU32,void*const*,int,VaU32); /* 1.6 T2: notifier quit + wake */

int _fltused;
void *memset(void *p,int value,unsigned n){volatile VaU8*d=p;while(n--)*d++=(VaU8)value;return p;}
static void *g_module;
static volatile long g_pending_error,g_error_count;
static VaU32 g_last_error;
static const VaGuid iid_27={0x8bcf1f58,0x9fe7,0x4583,{0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb}};
static const VaGuid iid_unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static int equal(const void*a,const void*b,unsigned n){const VaU8*x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}

void va_set_module(void *module){g_module=module;}
static void put_time(char*o,unsigned*k,unsigned long long t,unsigned long long f);
void va_log(const char *line){
    char stamped[320]; /* 1.6 T1: outage:/rebuild:/notify:/endpoint: lines end " t=<s.mmm>" (va_clock) */
    {VaU32 len=0;while(line[len])++len;
     static const char*const pre[4]={"outage: ","rebuild: ","notify: ","endpoint: "};int hit=0;
     for(int p=0;p<4 && !hit;++p){const char*a=pre[p];VaU32 i=0;while(a[i] && i<len && line[i]==a[i])++i;hit=!a[i];}
     if(hit && len>=2 && len<280 && line[len-2]=='\r' && line[len-1]=='\n'){
        unsigned k=0;for(VaU32 i=0;i<len-2;++i)stamped[k++]=line[i];
        unsigned long long f=1,t=va_clock(&f);if(!f)f=1;
        stamped[k++]=' ';stamped[k++]='t';stamped[k++]='=';put_time(stamped,&k,t,f);
        stamped[k++]='\r';stamped[k++]='\n';stamped[k]=0;line=stamped;
     }}
    VaU16 path[520];VaU32 n=GetModuleFileNameW(g_module,path,520);
    if(!n || n>=500)return;
    while(n && path[n-1]!='\\' && path[n-1]!='/')--n;
    const char *name="VinceAudio.log";
    while(*name)path[n++]=(VaU16)*name++;
    path[n]=0;
    void *file=CreateFileW(path,4,3,0,4,0x80,0);
    if(file==(void*)-1)return;
    VaU32 length=0,written;while(line[length])++length;
    WriteFile(file,line,length,&written,0);CloseHandle(file);
}

/* Call trace: switched at each VinceAudioCreate by VinceAudio.trace; the log handle stays open once on. */
int va_trace_on;
static void *g_trace_file;
static unsigned long long g_qpf;
static unsigned long long g_error_ticks;
typedef struct { char b[256]; unsigned n; } TraceLine;
/* Shift-subtract: no __aulldiv in a /nodefaultlib build. Shared with bridge_shadow.c (outage clock). */
unsigned long long udiv64(unsigned long long n,unsigned long long d,unsigned long long*rem){
    unsigned long long q=0,r=0;
    for(int i=63;i>=0;--i){r=(r<<1)|((n>>i)&1);if(r>=d){r-=d;q|=1ULL<<i;}}
    if(rem)*rem=r;return q;
}
static void put_c(TraceLine*l,char c){if(l->n<sizeof l->b-2)l->b[l->n++]=c;}
static void put_s(TraceLine*l,const char*s){while(*s)put_c(l,*s++);}
static void put_hex(TraceLine*l,VaU32 v){for(int i=28;i>=0;i-=4)put_c(l,"0123456789ABCDEF"[(v>>i)&15]);}
static void put_dec(TraceLine*l,unsigned long long v){
    char t[20];int k=0;
    do{if(v>>32){unsigned long long r;v=udiv64(v,10,&r);t[k++]=(char)('0'+r);}
       else{VaU32 w=(VaU32)v;t[k++]=(char)('0'+w%10);v=w/10;}}while(v);
    while(k)put_c(l,t[--k]);
}
/* v in thousandths -> "<v/1000>.<3 digits>" */
static void put_milli(TraceLine*l,unsigned long long v){
    unsigned long long r;put_dec(l,udiv64(v,1000,&r));VaU32 f=(VaU32)r;
    put_c(l,'.');put_c(l,(char)('0'+f/100));put_c(l,(char)('0'+f/10%10));put_c(l,(char)('0'+f%10));
}
/* va_log's stamp: ticks t at f per second -> "<s>.<mmm>" (o has room: va_log caps the body) */
static void put_time(char*o,unsigned*k,unsigned long long t,unsigned long long f){
    TraceLine l;l.n=0;unsigned long long r,s=udiv64(t,f,&r);put_milli(&l,s*1000+udiv64(r*1000,f,0));
    for(unsigned i=0;i<l.n;++i)o[(*k)++]=l.b[i];
}
static unsigned long long ticks_us(unsigned long long t){
    unsigned long long r,s=udiv64(t,g_qpf,&r);return s*1000000+udiv64(r*1000000,g_qpf,0);
}
/* Outage clock (1.4 T3): bridge_shadow.h. */
#ifdef VA_MOCK_BACKEND
typedef unsigned long long(VA_API*MockClockFn)(VaU32,unsigned long long*);
static MockClockFn g_mock_clock; /* resolved in initialize after LoadLibraryW */
unsigned long long va_clock(unsigned long long*freq){
    MockClockFn c=g_mock_clock;
    if(!c){if(freq)*freq=1;return 0;}
    return c(0,freq);
}
#else
unsigned long long va_clock(unsigned long long*freq){
    static unsigned long long f; /* benign race: every writer stores the same value */
    if(!f){long long q=0;QueryPerformanceFrequency(&q);f=q>0?(unsigned long long)q:1;}
    if(freq)*freq=f;
    long long t=0;QueryPerformanceCounter(&t);return (unsigned long long)t;
}
#endif
/* IEEE single as fixed point with integer math only; NaN/Inf/huge print as raw bits. */
static void put_float(TraceLine*l,VaU32 bits){
    int e=(int)((bits>>23)&255),s=e-150;
    if(e==255 || s>20){put_s(l,"0x");put_hex(l,bits);return;}
    if(bits>>31)put_c(l,'-');
    if(!e){put_s(l,"0.000");return;}
    unsigned long long v=(unsigned long long)((bits&0x7fffff)|0x800000)*1000;
    if(s>=0)v<<=s;else v=s<-40?0:(v+(1ULL<<(-s-1)))>>-s;
    put_milli(l,v);
}
/* The %-formats of va_trace (bridge_voice.h); shared with va_format. */
static void put_args(TraceLine*l,const char*fmt,__builtin_va_list ap){
    for(;*fmt;++fmt){
        if(*fmt!='%'){put_c(l,*fmt);continue;}
        if(!*++fmt)break;
        switch(*fmt){
        case 'x':put_hex(l,__builtin_va_arg(ap,VaU32));break;
        case 'p':put_hex(l,(VaU32)__builtin_va_arg(ap,const void*));break;
        case 'u':put_dec(l,__builtin_va_arg(ap,VaU32));break;
        case 'Q':put_dec(l,__builtin_va_arg(ap,unsigned long long));break;
        case 'f':put_float(l,__builtin_va_arg(ap,VaU32));break;
        case 'm':put_milli(l,ticks_us(__builtin_va_arg(ap,unsigned long long)));break;
        case 's':put_s(l,__builtin_va_arg(ap,const char*));break;
        default:put_c(l,*fmt);break;
        }
    }
}
void va_trace(const void*obj,const char*method,const char*fmt,...){
    if(!va_trace_on || !g_trace_file)return;
    TraceLine l;l.n=0;long long now;QueryPerformanceCounter(&now);
    put_s(&l,"T ");put_milli(&l,ticks_us((unsigned long long)now));put_c(&l,' ');
    put_hex(&l,GetCurrentThreadId());put_c(&l,' ');put_hex(&l,(VaU32)obj);put_c(&l,' ');put_s(&l,method);put_c(&l,' ');
    __builtin_va_list ap;__builtin_va_start(ap,fmt);put_args(&l,fmt,ap);__builtin_va_end(ap);
    l.b[l.n++]='\r';l.b[l.n++]='\n';
    VaU32 written;WriteFile(g_trace_file,l.b,l.n,&written,0);
}
unsigned va_format(char*out,unsigned cap,const char*fmt,...){
    if(!cap)return 0;
    TraceLine l;l.n=0;__builtin_va_list ap;__builtin_va_start(ap,fmt);put_args(&l,fmt,ap);__builtin_va_end(ap);
    unsigned n=l.n<cap-1?l.n:cap-1;for(unsigned i=0;i<n;++i)out[i]=l.b[i];out[n]=0;return n;
}
static int module_file(VaU16*path,const char*name){
    VaU32 n=GetModuleFileNameW(g_module,path,520);
    if(!n || n>=500)return 0;
    while(n && path[n-1]!='\\' && path[n-1]!='/')--n;
    while(*name)path[n++]=(VaU16)*name++;
    path[n]=0;return 1;
}
/* 3.1 T1: VinceAudio.ini [VinceAudio] MasterVolume=<0..1> (first 4 KB, first match wins), read at every create;
   0.5 when absent or invalid (5.1 T1: was 1.0). Strict: digits ['.' digits], >= 1 digit, nothing else. line = the one log line. */
float va_master_volume=0.5f; // was 1.0f
#ifdef VINCE_AIO /* AIO core (aio/core/core.c, 1.4 T7): [VinceAudio] of the core's VinceAIO.ini, read once at start-up (va_read_ini) */
#define VA_INI_NAME "VinceAIO.ini"
#else
#define VA_INI_NAME "VinceAudio.ini"
#endif
/* 3.1 T2: the game's master volume on a noshadow engine (m != 1 only; the real master holds game*m). */
float va_master_game_vol=1.0f;
static int ci_same(const char*a,VaU32 n,const char*b){
    for(VaU32 i=0;i<n;++i){char x=a[i],y=b[i];if(!y)return 0;if(x>='A' && x<='Z')x+=32;if(y>='A' && y<='Z')y+=32;if(x!=y)return 0;}
    return !b[n];
}
#ifdef VINCE_AIO
static void master_volume_read(const VaU16*path,char*line,unsigned cap){ /* path: "" or null = no file */
    const char*why="no " VA_INI_NAME;char text[36];text[0]=0;float v=0.5f; /* was 1.0f */int ok=0;
    void*f=path && path[0]?CreateFileW(path,0x80000000UL,3,0,3,0x80,0):(void*)-1;
#else
static void master_volume_read(char*line,unsigned cap){
    VaU16 path[520];const char*why="no " VA_INI_NAME;char text[36];text[0]=0;float v=0.5f; /* was 1.0f */int ok=0;
    void*f=module_file(path,VA_INI_NAME)?CreateFileW(path,0x80000000UL,3,0,3,0x80,0):(void*)-1;
#endif
    if(f!=(void*)-1){
        why=VA_INI_NAME " has no [VinceAudio] MasterVolume";
        char*b=HeapAlloc(GetProcessHeap(),0,4096);VaU32 n=0;
        if(!b || !ReadFile(f,b,4096,&n,0))n=0;
        CloseHandle(f);
        VaU32 i=n>=3 && (VaU8)b[0]==0xEF && (VaU8)b[1]==0xBB && (VaU8)b[2]==0xBF?3:0;int sect=0,found=0;
        while(i<n && !found){
            VaU32 s=i,e;while(i<n && b[i]!='\n')++i;e=i;if(i<n)++i;
            if(e>s && b[e-1]=='\r')--e;
            while(s<e && (b[s]==' ' || b[s]=='\t'))++s;while(e>s && (b[e-1]==' ' || b[e-1]=='\t'))--e;
            if(s==e || b[s]==';' || b[s]=='#')continue;
            if(b[s]=='['){sect=ci_same(b+s,e-s,"[VinceAudio]");continue;}
            VaU32 q=s;while(q<e && b[q]!='=')++q;
            if(!sect || q==e)continue;
            VaU32 k=q;while(k>s && (b[k-1]==' ' || b[k-1]=='\t'))--k;
            if(!ci_same(b+s,k-s,"MasterVolume"))continue;
            found=1;for(k=q+1;k<e && (b[k]==' ' || b[k]=='\t');++k){}
            VaU32 t=0;for(VaU32 j=k;j<e && t<32;++j)text[t++]=b[j];text[t]=0;
            /* ip saturates at 2; fr = the first 9 fraction digits (x 1e9); more = a nonzero digit beyond them */
            VaU32 ip=0,fr=0,nf=0,nd=0,more=0;
            for(;k<e && b[k]>='0' && b[k]<='9';++k,++nd){ip=ip*10+(VaU32)(b[k]-'0');if(ip>2)ip=2;}
            if(k<e && b[k]=='.'){for(++k;k<e && b[k]>='0' && b[k]<='9';++k,++nd)
                if(nf<9){fr=fr*10+(VaU32)(b[k]-'0');++nf;}else if(b[k]!='0')more=1;}
            if(!nd || k!=e)why="unparsable";
            else if(ip>1 || (ip==1 && (fr || more)))why="out of range";
            else{for(;nf<9;++nf)fr*=10;v=(float)((double)(int)ip+(double)(int)fr/1e9);ok=1;}
        }
        if(b)HeapFree(GetProcessHeap(),0,b);
    }
    va_master_volume=v;
    if(ok)va_format(line,cap,"master volume: %f (bits %x) from " VA_INI_NAME "\r\n",va_fbits(v),va_fbits(v));
    else if(why[0]=='u' || why[0]=='o')
        va_format(line,cap,"master volume: %f (bits %x) default: " VA_INI_NAME " MasterVolume '%s' %s\r\n",va_fbits(v),va_fbits(v),text,why);
    else va_format(line,cap,"master volume: %f (bits %x) default: %s\r\n",va_fbits(v),va_fbits(v),why);
}
#ifdef VINCE_AIO
static char va_mvline[160]; /* the master volume line, logged by every create */
void va_read_ini(const VaU16*path){master_volume_read(path,va_mvline,sizeof va_mvline);}
#endif
static void trace_switch(void){
    VaU16 path[520];
    int on=module_file(path,"VinceAudio.trace") && GetFileAttributesW(path)!=0xFFFFFFFFUL;
    if(on && !g_trace_file){
        long long f=0;void *file=(void*)-1;
        if(QueryPerformanceFrequency(&f) && f>0 && module_file(path,"VinceAudio.log"))file=CreateFileW(path,4,3,0,4,0x80,0);
        if(file==(void*)-1){va_trace_on=0;return;}
        g_qpf=(unsigned long long)f;g_trace_file=file;va_trace_on=1;
        va_trace(0,"TraceStart","qpf=%Q",g_qpf);
    }
    va_trace_on=on;
}
void va_report_pending_errors(void){
    if(!__atomic_exchange_n(&g_pending_error,0,__ATOMIC_ACQ_REL))return;
    char line[]="XAudio2 critical error 0x00000000: automatic routing failed; full recovery is not implemented.\r\n";
    VaU32 error=__atomic_load_n(&g_last_error,__ATOMIC_ACQUIRE),hr=error;
    unsigned offset=sizeof("XAudio2 critical error 0x")-1;
    for(int i=7;i>=0;--i){line[offset+i]="0123456789ABCDEF"[error&15];error>>=4;}
    va_log(line);
    if(va_trace_on){
        long long now;QueryPerformanceCounter(&now);
        VA_TRACE(0,"CriticalErrorDrain","hr=0x%x callback=%m drain=%m",hr,
                 __atomic_load_n(&g_error_ticks,__ATOMIC_ACQUIRE),(unsigned long long)now);
    }
}
VaU32 VA_API VinceAudioCriticalErrorCount(void){return (VaU32)__atomic_load_n(&g_error_count,__ATOMIC_ACQUIRE);}

static void VA_API processing_start(void *self){(void)self;}
static void VA_API processing_end(void *self){(void)self;}
static void VA_API critical_error(void *self,VaHR hr){
    VaEngine *e=(VaEngine*)((VaU8*)self-__builtin_offsetof(VaEngine,callback_vt)); /* self = &e->callback_vt */
    if(va_trace_on){long long now;QueryPerformanceCounter(&now);__atomic_store_n(&g_error_ticks,(unsigned long long)now,__ATOMIC_RELEASE);}
    __atomic_store_n(&g_last_error,(VaU32)hr,__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_error_count,1,__ATOMIC_RELAXED);
    __atomic_store_n(&g_pending_error,1,__ATOMIC_RELEASE);
    va_sh_signal(&e->sh,VA_LOST_CRITICAL,0,hr); /* outage engine only; atomics only on this thread */
}
static void *callback_vt[]={(void*)processing_start,(void*)processing_end,(void*)critical_error};

/* Shadow registry. g_cs_state 0 -> 1 (one thread initialises) -> 2 ready; g_shadow_engines>0 only after 2,
   so va_lock never enters an uninitialised section. Without VinceAudio.shadow: one atomic load, no lock.
   Policy (T3): g_cs guards registry state only (nodes, g_shadow_chain) and is never held across a real XAudio2
   call: if 2.9 holds an engine lock while it runs voice callbacks (undocumented, R1.3-005; the mock does), and a
   callback re-enters a wrapper, it would wait
   on g_cs while the g_cs holder waits on that engine lock (deadlock, test_events case_lock_reentry). */
static VaCriticalSection g_cs;
static volatile long g_cs_state,g_shadow_engines;
static VaEngine *g_shadow_chain;
static void cs_ready(void){
    long expected=0;
    if(__atomic_compare_exchange_n(&g_cs_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        InitializeCriticalSection(&g_cs);__atomic_store_n(&g_cs_state,2,__ATOMIC_RELEASE);return;
    }
    while(__atomic_load_n(&g_cs_state,__ATOMIC_ACQUIRE)!=2)Sleep(0);
}
int va_lock(void){if(!__atomic_load_n(&g_shadow_engines,__ATOMIC_ACQUIRE))return 0;EnterCriticalSection(&g_cs);return 1;}
void va_unlock(int taken){if(taken)LeaveCriticalSection(&g_cs);}
void va_shadow_forget(void *wrapper){
    for(VaEngine*e=g_shadow_chain;e;e=e->next_shadow)
        for(VaShadowNode**p=&e->nodes;*p;p=&(*p)->next)
            if((*p)->wrapper==wrapper){
                VaShadowNode*n=*p;*p=n->next;((VaVoice*)wrapper)->rec=0;va_sh_rec_free(n->rec,1);
                if(e->sh.rb_job==n)e->sh.rb_job=0; /* 1.5 T2: a job in flight for it: the claimant destroys its result */
                if(e->sh.wk_mn==n)e->sh.wk_mn=0; /* 2.2 T2: the attempt's master node went: its new master is orphaned */
                if(n->rb_real){VaParked*q=HeapAlloc(GetProcessHeap(),0,sizeof *q);if(q){q->real=n->rb_real;q->next=e->sh.rb_gone;e->sh.rb_gone=q;}}
                HeapFree(GetProcessHeap(),0,n);return;
            }
}
VaShadowRec *va_shadow_find(const void *wrapper){
    for(VaEngine*e=g_shadow_chain;e;e=e->next_shadow)
        for(VaShadowNode*n=e->nodes;n;n=n->next)if(n->wrapper==wrapper)return n->rec;
    return 0;
}
/* ---- Default-device notifier (1.5 T4): one per process while a VinceAudio.rebuild engine lives (g_nt_refs; started by
   VinceAudioCreate, stopped by the last such engine's final Release; never in DllMain). Its thread registers a static
   IMMNotificationClient whose callbacks only write the atomics below (no lock, wait, COM or log); engines poll them
   (va_nt_poll on game threads, va_rebuild). gen/last: eRender default changes (last = va_clock at the change); present:
   -1 unknown, 0 no default render endpoint, 1 one; appear: a render default arrived, a device was added or became
   active. g_nt_lock (spin) guards refs and the thread only; g_cs is never taken here. Mock build: no COM, the
   enumerator is mock_xaudio2_9.dll's MockDeviceEnumerator. ---- */
VA_IMP void *VA_API CreateThread(void*,VaU32,VaU32(VA_API*)(void*),void*,VaU32,VaU32*);
VA_IMP void *VA_API CreateEventW(void*,int,int,const VaU16*);
VA_IMP int VA_API SetEvent(void*);
VA_IMP void VA_API GetLocalTime(VaU16*); /* 1.6 T1: SYSTEMTIME, 8 x u16 */
#ifndef VA_MOCK_BACKEND
VA_IMP VaHR VA_API CoInitializeEx(void*,VaU32);
VA_IMP VaHR VA_API CoCreateInstance(const VaGuid*,void*,VaU32,const VaGuid*,void**);
VA_IMP void VA_API CoUninitialize(void);
VA_IMP void VA_API CoTaskMemFree(void*);   /* 1.6 T1: endpoint snapshot */
VA_IMP VaHR VA_API PropVariantClear(void*);
static const VaGuid clsid_enumerator={0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const VaGuid iid_enumerator={0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
#endif
static const VaGuid iid_notify={0x7991EEC9,0x7E89,0x4D85,{0x83,0x90,0x6C,0x70,0x3C,0xEC,0x60,0xC0}};
static volatile long g_nt_gen,g_nt_appear,g_nt_present=-1,g_nt_lock,g_nt_refs,g_nt_hr;
static unsigned long long g_nt_last;
static void *g_nt_thread,*g_nt_quit,*g_nt_ready;
/* 1.6 T2: redundant default changes. con = hash of the last eConsole render default id (0 = NULL), cgen counts those
   changes. A Live sets live_want = its seq (live_cgen = cgen then) and wakes the notifier, which reads the role-0 id
   and stores live = seq<<32|hash (hash 0: unknown, or a change since that Live). va_nt_poll skips a burst whose con
   equals the Live's id. */
static volatile long g_nt_con,g_nt_cgen,g_nt_live_seq,g_nt_live_want,g_nt_live_cgen;
static unsigned long long g_nt_live;
static void *g_nt_wake;
/* FNV-1a 32 over UTF-16 units, ASCII case-folded, forced nonzero; NULL -> 0 */
static VaU32 nt_hash(const VaU16*id){
    if(!id)return 0;
    VaU32 h=2166136261u;for(;*id;++id){VaU32 c=*id;if(c>='A' && c<='Z')c+=32;h=(h^(c&0xFF))*16777619u;h=(h^(c>>8))*16777619u;}
    return h?h:1;
}
#define NT_VT(o,i) (((void**)*(void**)(o))[i])
typedef struct { VaGuid fmtid; VaU32 pid; } VaPropKey; /* PROPERTYKEY, passed by value */
static VaHR VA_API nt_qi(void*self,const VaGuid*iid,void**out){
    if(!out)return VA_E_INVALIDARG;
    if(iid && (equal(iid,&iid_unknown,sizeof *iid) || equal(iid,&iid_notify,sizeof *iid))){*out=self;return 0;}
    *out=0;return VA_E_NOINTERFACE;
}
static VaU32 VA_API nt_ref(void*self){(void)self;return 1;} /* static object: AddRef and Release */
/* 1.6 T1: every callback also appends one event to this ring (CAS-reserved head, slot seq published last; full ->
   overflow count); the notifier thread drains it into notify: lines. Callbacks: atomics and copies only.
   1.6 T8: property events are counted (g_nt_props, one cumulative 'notify: property N' line per pass), not queued:
   they flooded the 64 slots (928 of 1442 lines in the 1.6 V2 log) and dropped default/state lines. */
enum{NT_DEFAULT,NT_ADDED,NT_REMOVED,NT_STATE,NT_PROPERTY};
typedef struct { VaU32 seq,kind,flow,role,state; char id[64]; } NtEv;
/* 1.6 T10: 256 slots (was 64): a Windows Audio restart burst is one state+removed (then added+state) pair per endpoint
   within ~10 ms, before any drain; the 1.6 T8 re-run log (raw/VinceAudio-1.6-rerun-T8-20261021.log L196, L267) queued 64
   and dropped 17, then 15 more (cumulative 32): ~81 events, 96 at most; 256 is > 2.5x that. Power of two (masks). */
#define NT_RING 256u
static NtEv g_nt_ring[NT_RING];
static volatile VaU32 g_nt_head,g_nt_tail,g_nt_over;
static volatile VaU32 g_nt_props; /* 1.6 T8: property callbacks, counted only */
static volatile long g_nt_req; /* 1.6 T1: endpoint lines wanted (exchanged to 0 by the notifier thread) */
static void nt_push(VaU32 kind,VaU32 flow,VaU32 role,VaU32 state,const VaU16*id){
    VaU32 h=__atomic_load_n(&g_nt_head,__ATOMIC_ACQUIRE);
    for(;;){
        if(h-__atomic_load_n(&g_nt_tail,__ATOMIC_ACQUIRE)>=NT_RING){__atomic_add_fetch(&g_nt_over,1,__ATOMIC_ACQ_REL);return;}
        if(__atomic_compare_exchange_n(&g_nt_head,&h,h+1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
    }
    NtEv*e=&g_nt_ring[h&(NT_RING-1)];e->kind=kind;e->flow=flow;e->role=role;e->state=state;
    unsigned i=0;if(id)for(;i<63 && id[i];++i)e->id[i]=id[i]<0x20 || id[i]>0x7E?'?':(char)id[i];
    e->id[i]=0;
    __atomic_store_n(&e->seq,h+1,__ATOMIC_RELEASE);
}
static VaHR VA_API nt_state(void*self,const VaU16*id,VaU32 state){
    (void)self;nt_push(NT_STATE,0,0,state,id);
    if(state==1)__atomic_add_fetch(&g_nt_appear,1,__ATOMIC_ACQ_REL);return 0; /* DEVICE_STATE_ACTIVE */
}
static VaHR VA_API nt_added(void*self,const VaU16*id){(void)self;nt_push(NT_ADDED,0,0,0,id);__atomic_add_fetch(&g_nt_appear,1,__ATOMIC_ACQ_REL);return 0;}
static VaHR VA_API nt_removed(void*self,const VaU16*id){(void)self;nt_push(NT_REMOVED,0,0,0,id);return 0;}
static VaHR VA_API nt_default(void*self,VaU32 flow,VaU32 role,const VaU16*id){
    (void)self;nt_push(NT_DEFAULT,flow,role,0,id);
    if(flow)return 0; /* eRender (0) only; every role counts */
    __atomic_store_n(&g_nt_last,va_clock(0),__ATOMIC_RELEASE);__atomic_store_n(&g_nt_present,id?1:0,__ATOMIC_RELEASE);
    if(id)__atomic_add_fetch(&g_nt_appear,1,__ATOMIC_ACQ_REL);
    if(!role){__atomic_store_n(&g_nt_con,(long)nt_hash(id),__ATOMIC_RELEASE);__atomic_add_fetch(&g_nt_cgen,1,__ATOMIC_ACQ_REL);} /* 1.6 T2 */
    __atomic_add_fetch(&g_nt_gen,1,__ATOMIC_ACQ_REL); /* last: a poll that sees this gen sees its time */
    return 0;
}
static VaHR VA_API nt_property(void*self,const VaU16*id,VaPropKey key){(void)self;(void)key;(void)id;__atomic_add_fetch(&g_nt_props,1,__ATOMIC_ACQ_REL);return 0;}
static void *nt_vtbl[8]={(void*)nt_qi,(void*)nt_ref,(void*)nt_ref,(void*)nt_state,(void*)nt_added,(void*)nt_removed,
    (void*)nt_default,(void*)nt_property};
static void **nt_client=nt_vtbl; /* the object is &nt_client; enumerator Register/Unregister take no reference */
typedef VaHR(VA_API*NtDefault)(void*,VaU32,VaU32,void**);
typedef VaHR(VA_API*NtRegister)(void*,void*);
typedef VaU32(VA_API*NtRelease)(void*);
/* 1.6 T1, notifier thread only: the ring into notify: lines; 1 = any event taken. */
static int nt_drain(void){
    static const char*const kinds[5]={"default","added","removed","state","property"};
    static VaU32 over_seen,props_seen;int got=0;char line[160];
    for(;;){
        VaU32 t=__atomic_load_n(&g_nt_tail,__ATOMIC_RELAXED);NtEv*e=&g_nt_ring[t&(NT_RING-1)];
        if(__atomic_load_n(&e->seq,__ATOMIC_ACQUIRE)!=t+1)break;
        unsigned n=va_format(line,sizeof line,"notify: %s flow=%u role=%u state=%x id=%s\r\n",
                             kinds[e->kind<5?e->kind:0],e->flow,e->role,e->state,e->id);
        __atomic_store_n(&g_nt_tail,t+1,__ATOMIC_RELEASE);
        if(n)va_log(line);got=1;
    }
    VaU32 o=__atomic_load_n(&g_nt_over,__ATOMIC_ACQUIRE);
    if(o!=over_seen){over_seen=o;if(va_format(line,sizeof line,"notify: overflow %u\r\n",o))va_log(line);}
    VaU32 p=__atomic_load_n(&g_nt_props,__ATOMIC_ACQUIRE);
    if(p!=props_seen){props_seen=p;if(va_format(line,sizeof line,"notify: property %u\r\n",p))va_log(line);}
    return got;
}
static const VaGuid iid_audio_client={0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const VaPropKey pkey_friendly_name={{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}},14};
static void nt_ascii(char*o,const VaU16*w){unsigned i=0;if(w)for(;i<63 && w[i];++i)o[i]=w[i]<0x20 || w[i]>0x7E?'?':(char)w[i];o[i]=0;}
/* 1.6 T1, notifier thread only: one endpoint: line per role (eConsole, eMultimedia), as device_probe.c snapshot().
   1.6 T2: want != 0 (a Live's seq): role 0's id hash goes to g_nt_live before its line is logged. */
static void nt_endpoints(void*en,long want){
    typedef VaHR(VA_API*Activate)(void*,const VaGuid*,VaU32,void*,void**);typedef VaHR(VA_API*OpenStore)(void*,VaU32,void**);
    typedef VaHR(VA_API*GetId)(void*,VaU16**);typedef VaHR(VA_API*GetValue)(void*,const VaPropKey*,void*);
    typedef VaHR(VA_API*GetMix)(void*,VaU8**);
    static const char*const roles[2]={"console","multimedia"};
    for(VaU32 role=0;role<2;++role){
        char line[256],id[64],name[64];void*dev=0;id[0]=name[0]=0;
        VaHR hr=((NtDefault)NT_VT(en,4))(en,0,role,&dev);
        VaU32 hash=0; /* 1.6 T2 */
        if(want && !role && (hr<0 || !dev))__atomic_store_n(&g_nt_live,(unsigned long long)(VaU32)want<<32,__ATOMIC_RELEASE);
        if(hr<0 || !dev){if(va_format(line,sizeof line,"endpoint: %s none 0x%x\r\n",roles[role],(VaU32)(hr<0?hr:VA_E_FAIL)))va_log(line);continue;}
        VaU16*wid=0;
        if(((GetId)NT_VT(dev,5))(dev,&wid)>=0 && wid){nt_ascii(id,wid);hash=nt_hash(wid);
#ifndef VA_MOCK_BACKEND
            CoTaskMemFree(wid);
#endif
        }
        if(want && !role){ /* 1.6 T2: unknown when a role-0 default change came after that Live */
            if(__atomic_load_n(&g_nt_cgen,__ATOMIC_ACQUIRE)!=__atomic_load_n(&g_nt_live_cgen,__ATOMIC_ACQUIRE))hash=0;
            __atomic_store_n(&g_nt_live,(unsigned long long)(VaU32)want<<32|hash,__ATOMIC_RELEASE);
        }
        void*store=0;
        if(((OpenStore)NT_VT(dev,4))(dev,0,&store)>=0 && store){
            VaU8 pv[16];memset(pv,0,sizeof pv);
            if(((GetValue)NT_VT(store,5))(store,&pkey_friendly_name,pv)>=0 && (pv[0]|pv[1]<<8)==31) /* VT_LPWSTR */
                nt_ascii(name,*(const VaU16**)(pv+8));
#ifndef VA_MOCK_BACKEND
            PropVariantClear(pv);
#endif
            ((NtRelease)NT_VT(store,2))(store);
        }
        void*client=0;VaU32 tag=0,ch=0,rate=0;
        VaHR mix=((Activate)NT_VT(dev,3))(dev,&iid_audio_client,0x17 /* CLSCTX_ALL */,0,&client);
        if(mix>=0 && client){
            VaU8*wfx=0;mix=((GetMix)NT_VT(client,8))(client,&wfx);
            if(mix>=0 && wfx){tag=wfx[0]|wfx[1]<<8;ch=wfx[2]|wfx[3]<<8;rate=wfx[4]|wfx[5]<<8|wfx[6]<<16|(VaU32)wfx[7]<<24;
#ifndef VA_MOCK_BACKEND
                CoTaskMemFree(wfx);
#endif
            }
            else if(mix>=0)mix=VA_E_FAIL;
            ((NtRelease)NT_VT(client,2))(client);
        }
        else if(mix>=0)mix=VA_E_FAIL;
        ((NtRelease)NT_VT(dev,2))(dev);
        unsigned n=va_format(line,sizeof line,"endpoint: %s id=%s name=\"%s\" ",roles[role],id,name);
        if(mix<0)n+=va_format(line+n,sizeof line-n,"mix hr=%x\r\n",(VaU32)mix);
        else n+=va_format(line+n,sizeof line-n,"rate=%u ch=%u tag=%x\r\n",rate,ch,tag);
        va_log(line);
    }
}
static VaU32 VA_API nt_thread(void*arg){
    void*quit=arg,*ready=g_nt_ready,*en=0;VaHR hr;
#ifdef VA_MOCK_BACKEND
    typedef VaHR(VA_API*MockEnum)(void**);VaU16 path[520];
    void*m=module_file(path,"mock_xaudio2_9.dll")?LoadLibraryW(path):0;
    MockEnum me=m?(MockEnum)GetProcAddress(m,"MockDeviceEnumerator"):0;
    hr=me?me(&en):VA_E_FAIL;
#else
    VaHR init=CoInitializeEx(0,0); /* COINIT_MULTITHREADED */
    hr=init<0?init:CoCreateInstance(&clsid_enumerator,0,0x17 /* CLSCTX_ALL */,&iid_enumerator,&en);
#endif
    if(hr<0)en=0;
    if(en){
        void*dev=0;VaHR d=((NtDefault)NT_VT(en,4))(en,0,0,&dev); /* eRender, eConsole: the initial present */
        __atomic_store_n(&g_nt_present,d>=0?1:d==(VaHR)0x80070490L?0:-1,__ATOMIC_RELEASE);
        if(d>=0 && dev)((NtRelease)NT_VT(dev,2))(dev);
        hr=((NtRegister)NT_VT(en,6))(en,&nt_client);
    }
    __atomic_store_n(&g_nt_hr,hr,__ATOMIC_RELEASE);SetEvent(ready);
    if(hr>=0){ /* 1.6 T1: 100 ms passes drain the ring; endpoint lines after a burst or on request; last pass at quit */
        void*const wait[2]={quit,g_nt_wake}; /* 1.6 T2: a Live's wake runs the pass at once */
        for(int had=0;;){
            int q=(wait[1]?WaitForMultipleObjects(2,wait,0,100):WaitForSingleObject(quit,100))==0,got=nt_drain();
            long req=__atomic_exchange_n(&g_nt_req,0,__ATOMIC_ACQ_REL);
            long want=__atomic_exchange_n(&g_nt_live_want,0,__ATOMIC_ACQ_REL); /* after req: a Live stores want first */
            if(req || want || (!q && had && !got))nt_endpoints(en,want);
            if(q)break;
            had=got;
        }
        ((NtRegister)NT_VT(en,7))(en,&nt_client);
    }
    if(en)((NtRelease)NT_VT(en,2))(en);
#ifndef VA_MOCK_BACKEND
    if(init==0 || init==1)CoUninitialize(); /* S_OK / S_FALSE only */
#endif
    return 0;
}
static void nt_lock(void){long z=0;while(!__atomic_compare_exchange_n(&g_nt_lock,&z,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){z=0;Sleep(1);}}
static void nt_unlock(void){__atomic_store_n(&g_nt_lock,0,__ATOMIC_RELEASE);}
/* VinceAudioCreate with the rebuild switch: the first engine starts the thread and waits <= 2000 ms for its registration
   result; a failure only logs (rebuild works without the notifier). */
static void nt_start(void){
    nt_lock();
    if(g_nt_refs++){nt_unlock();return;}
    VaHR hr=VA_E_FAIL;__atomic_store_n(&g_nt_present,-1,__ATOMIC_RELEASE);
    g_nt_quit=CreateEventW(0,1,0,0);g_nt_ready=CreateEventW(0,1,0,0);g_nt_wake=CreateEventW(0,0,0,0); /* 1.6 T2: auto-reset */
    if(g_nt_quit && g_nt_ready)g_nt_thread=CreateThread(0,0,nt_thread,g_nt_quit,0,0);
    if(g_nt_thread)hr=WaitForSingleObject(g_nt_ready,2000)==0?__atomic_load_n(&g_nt_hr,__ATOMIC_ACQUIRE)
        :(VaHR)0x800705B4L; /* timeout: the thread stays until the stop */
    nt_unlock();
    char line[64];
    if(hr>=0)va_log("notify: on\r\n");
    else if(va_format(line,sizeof line,"notify: unavailable 0x%x\r\n",(VaU32)hr))va_log(line);
}
/* The last rebuild engine's final Release, unlocked: quit, join (5 s), close. */
static void nt_stop(void){
    nt_lock();
    if(--g_nt_refs){nt_unlock();return;}
    void*t=g_nt_thread;int stuck=0;g_nt_thread=0;
    if(t){SetEvent(g_nt_quit);stuck=WaitForSingleObject(t,5000)!=0;CloseHandle(t);}
    if(!stuck){if(g_nt_quit)CloseHandle(g_nt_quit);if(g_nt_ready)CloseHandle(g_nt_ready);if(g_nt_wake)CloseHandle(g_nt_wake);} /* stuck: they leak, the thread keeps them */
    g_nt_quit=g_nt_ready=g_nt_wake=0;
    nt_unlock();
    if(stuck)va_log("notify: thread did not stop\r\n");
    if(t)va_log("notify: off\r\n");
}
/* 1.6 T2: an engine went Live (create or rebuild): record it, ask the notifier for the role-0 id now. want and
   live_cgen go before the g_nt_req store (1.6 T1 endpoint lines), so a pass that takes req also takes want; then wake. */
static void nt_live(VaShadowEng*s){
    long seq=__atomic_add_fetch(&g_nt_live_seq,1,__ATOMIC_ACQ_REL);if(!seq)seq=__atomic_add_fetch(&g_nt_live_seq,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&s->live_t,va_clock(0),__ATOMIC_RELEASE);__atomic_store_n(&s->live_seq,seq,__ATOMIC_RELEASE);
    __atomic_store_n(&g_nt_live_cgen,__atomic_load_n(&g_nt_cgen,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
    __atomic_store_n(&g_nt_live_want,seq,__ATOMIC_RELEASE);__atomic_store_n(&g_nt_req,1,__ATOMIC_RELEASE);
    void*w=__atomic_load_n(&g_nt_wake,__ATOMIC_ACQUIRE);if(w)SetEvent(w);
}
void va_nt_poll(VaShadowEng*s){
    if(!s->rebuild || __atomic_load_n(&s->lost,__ATOMIC_SEQ_CST))return;
    long g=__atomic_load_n(&g_nt_gen,__ATOMIC_ACQUIRE),seen=__atomic_load_n(&s->nt_seen,__ATOMIC_ACQUIRE);
    if(g==seen)return;
    unsigned long long f=1,now=va_clock(&f),last=__atomic_load_n(&g_nt_last,__ATOMIC_ACQUIRE);if(!f)f=1;
    if(now<last || now-last<udiv64(f*100,1000,0))return; /* 100 ms quiet: one Lost per burst */
    { /* 1.6 T2: the default is still the endpoint read after this engine's last Live: no Lost */
        unsigned long long rec=__atomic_load_n(&g_nt_live,__ATOMIC_ACQUIRE),lt=__atomic_load_n(&s->live_t,__ATOMIC_ACQUIRE);
        VaU32 seq=(VaU32)__atomic_load_n(&s->live_seq,__ATOMIC_ACQUIRE);
        if(seq && (VaU32)(rec>>32)!=seq && now>=lt && now-lt<udiv64(f*300,1000,0))return; /* the read is due: wait <= 300 ms */
        if(seq && (VaU32)(rec>>32)==seq && (VaU32)rec && (VaU32)rec==(VaU32)__atomic_load_n(&g_nt_con,__ATOMIC_ACQUIRE)){
            if(__atomic_compare_exchange_n(&s->nt_seen,&seen,g,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
                char line[96];unsigned k=0;static const char pre[]="notify: default unchanged since Live ";
                for(unsigned i=0;pre[i];++i)line[k++]=pre[i];
                put_time(line,&k,lt,f);line[k++]='\r';line[k++]='\n';line[k]=0;va_log(line);
            }
            return;
        }
    }
    if(__atomic_compare_exchange_n(&s->nt_seen,&seen,g,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))va_sh_signal(s,VA_LOST_DEFAULT,0,0);
}
/* Outage (1.4 T2): eng_lost takes a pending signal (once, under g_cs) and logs it unlocked; 1 = Lost.
   eng_failed: a fatal real HRESULT on an outage engine signals Lost and becomes S_OK. */
volatile long va_outage_refs;
/* 1.5 T1: self = the caller's own count in sh.calls (1 = a counted t_* entry, 0 = uncounted, -1 = never a rebuild attempt). */
static int eng_lost_self(VaEngine*e,int self){
    if(!e->sh.outage)return 0;
    if(self>=0)va_nt_poll(&e->sh); /* 1.5 T4: a default-device change signals Lost (Live rebuild engine) */
    char line[128];int k=va_lock(),n=va_sh_take(&e->sh,line,sizeof line),lost=(int)e->sh.lost;va_unlock(k);
    if(n)va_log(line);
    if(lost && self>=0 && e->sh.rebuild){va_rebuild(&e->sh,self);lost=(int)__atomic_load_n(&e->sh.lost,__ATOMIC_SEQ_CST);}
    return lost;
}
static int eng_lost(VaEngine*e){return eng_lost_self(e,0);}
static VaHR eng_failed(VaEngine*e,const char*method,VaHR hr){
    if(!e->sh.outage || !va_sh_fatal(hr))return hr;
    va_sh_signal(&e->sh,VA_LOST_FAILED,method,hr);return eng_lost_self(e,-1)?0:hr;
}
#define ELOST(e) ((e)->sh.outage && eng_lost_self(e,1)) /* Lost policy: no real call, S_OK (callers counted, t_*) */
/* 1.5 T1 in-flight guard: t_* entries that use e->real count themselves on an outage engine. */
static void eng_in(VaEngine*e){if(e->sh.outage)__atomic_add_fetch(&e->sh.calls,1,__ATOMIC_SEQ_CST);}
static void eng_out(VaEngine*e){if(e->sh.outage)__atomic_sub_fetch(&e->sh.calls,1,__ATOMIC_SEQ_CST);}
/* Wrap a new real voice and, on a shadow engine, append its node. On failure returns 0 with *doomed=real:
   the caller destroys it after va_unlock. */
static void *adopt(VaEngine*e,void*real,unsigned kind,void**doomed){
    void *w=va_wrap_voice(real,kind);
    if(!w){*doomed=real;return 0;}
    if(!e->shadow)return w;
    VaShadowNode *n=HeapAlloc(GetProcessHeap(),8,sizeof *n);
    if(n)n->rec=va_sh_rec_new(&e->sh,kind);
    if(!n || !n->rec){if(n)HeapFree(GetProcessHeap(),0,n);HeapFree(GetProcessHeap(),0,w);*doomed=real;return 0;}
    n->engine=e;n->wrapper=w;n->kind=kind;((VaVoice*)w)->rec=n->rec;
    VaShadowNode **p=&e->nodes;while(*p)p=&(*p)->next;*p=n;
    if(!real){__atomic_add_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);va_sh_silence(n->rec);} /* silent wrapper: slow path until its DestroyVoice */
    return w;
}
static void destroy_real(void*real){typedef void(VA_API*F)(void*);if(real)((F)(*(void***)real)[18])(real);}
VaHR VA_API VinceAudioShadowVoices(void*engine,VaU32 capacity,void**voices,VaU32*kinds,VaU32*count){
    if(!count)return VA_E_INVALIDARG;
    int k=va_lock();VaHR hr=0;VaU32 total=0;
    if(!engine){for(VaEngine*e=g_shadow_chain;e;e=e->next_shadow)for(VaShadowNode*n=e->nodes;n;n=n->next)++total;}
    else{
        VaEngine*e=g_shadow_chain;while(e && e!=engine)e=e->next_shadow;
        if(!e)hr=VA_E_FAIL;
        else for(VaShadowNode*n=e->nodes;n;n=n->next,++total)
            if(total<capacity){if(voices)voices[total]=n->wrapper;if(kinds)kinds[total]=n->kind;}
    }
    if(hr>=0)*count=total;
    va_unlock(k);return hr;
}

static VaU32 VA_API addref(VaEngine*e){if(e->sh.outage)eng_lost(e);return (VaU32)__atomic_add_fetch(&e->refs,1,__ATOMIC_RELAXED);}
static VaHR VA_API query(VaEngine*e,const VaGuid*iid,void**out){
    if(!out || !iid)return VA_E_INVALIDARG;
    if(e->sh.outage)eng_lost(e);
    *out=0;
    if(!equal(iid,&iid_27,sizeof *iid) && !equal(iid,&iid_unknown,sizeof *iid))return VA_E_NOINTERFACE;
    addref(e);*out=e;return 0;
}
static VaU32 VA_API release(VaEngine*e){
    if(e->sh.outage)eng_lost_self(e,-1); /* 1.5 T1: no rebuild attempt at Release */
    long left=__atomic_sub_fetch(&e->refs,1,__ATOMIC_ACQ_REL);
    if(left){VA_TRACE(e,"Release","-> %u",(VaU32)left);return (VaU32)left;}
    int leak=0;
    if(e->sh.rebuild){ /* 1.6 T3: closing (rb_busy 0->2, no attempt claims after), then wait out a running attempt and its
                          caller's eng_out (calls <= 0); 5000 ms real time (QPC, not va_clock: MockClock in the mock build).
                          2.2 T2: a published worker result is taken (4->2) and released here; on the bound a job still with
                          the worker is abandoned (3->2): the worker discards its objects, e leaks. */
        long long q0=0,q=0,qf=0;QueryPerformanceFrequency(&qf);if(qf<=0)qf=1;QueryPerformanceCounter(&q0);
        int waited=0,timeout=0,took=0;
        for(;;){
            long z=0;
            if(__atomic_compare_exchange_n(&e->sh.rb_busy,&z,2,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))z=2;
            else if(z==4 && __atomic_compare_exchange_n(&e->sh.rb_busy,&z,2,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){z=2;took=1;}
            if(z==2 && __atomic_load_n(&e->sh.calls,__ATOMIC_SEQ_CST)<=0)break;
            QueryPerformanceCounter(&q);
            if((unsigned long long)(q-q0)>=udiv64((unsigned long long)qf*5000,1000,0)){
                z=3;__atomic_compare_exchange_n(&e->sh.rb_busy,&z,2,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED); /* 2.2 T2: abandon */
                timeout=1;break;
            }
            waited=1;Sleep(1);
        }
        if(timeout){va_log("rebuild: release wait timed out (5000 ms), engine leaked\r\n");return 0;} /* the attempt still owns e */
        if(waited){
            QueryPerformanceCounter(&q);char line[64];
            if(va_format(line,sizeof line,"rebuild: release waited %u ms (bound 5000)\r\n",(VaU32)udiv64((unsigned long long)(q-q0)*1000,(unsigned long long)qf,0)))va_log(line);
        }
        if(took){ /* 2.2 T2: the worker's published engine and master, never handed to the graph */
            typedef void(VA_API*Unregister)(void*,void*);typedef VaU32(VA_API*Release)(void*);
            void*r=e->sh.wk_real;destroy_real(e->sh.wk_m);e->sh.wk_real=e->sh.wk_m=0;
            if(r){((Unregister)(*(void***)r)[4])(r,&e->callback_vt);((Release)(*(void***)r)[2])(r);}
        }
        if(e->sh.wk_thread){ /* 2.2 T2: stop the idle worker: quit, wake, join (5 s), close */
            void*t=e->sh.wk_thread;e->sh.wk_thread=0;
            __atomic_store_n(&e->sh.wk_quit,1,__ATOMIC_SEQ_CST);SetEvent(e->sh.wk_wake);
            int stuck=WaitForSingleObject(t,5000)!=0;CloseHandle(t);
            if(!stuck){CloseHandle(e->sh.wk_wake);e->sh.wk_wake=0;va_log("rebuild: worker off\r\n");} /* stuck: the wake leaks, the thread keeps it */
            else{leak=1;va_log("rebuild: worker did not stop\r\n");}
        }
    }
    VaParked *parked=0;
    if(e->sh.thread){ /* 1.4 T4 D8: stop the outage thread unlocked; stuck or self -> e and its shims leak */
        __atomic_store_n(&e->sh.quit,1,__ATOMIC_SEQ_CST);
        if(GetCurrentThreadId()==e->sh.thread_id)leak=1;
        else if(WaitForSingleObject(e->sh.thread,5000)!=0){leak=1;va_log("outage: callback thread did not stop\r\n");}
        CloseHandle(e->sh.thread);e->sh.thread=0;
    }
    if(e->shadow){
        char line[1024];
        int k=va_lock();
        va_sh_summary(&e->sh,line,sizeof line); /* before the nodes go; logged after va_unlock */
        for(VaShim*s=e->sh.shims;s;s=s->next)s->rec=0; /* 1.4 T4: records go below */
        /* Nodes and records only: wrappers and real voices stay as the game left them (unshadowed from now on). */
        while(e->nodes){
            VaShadowNode*n=e->nodes;e->nodes=n->next;((VaVoice*)n->wrapper)->rec=0;
            va_sh_rec_free(n->rec,0);HeapFree(GetProcessHeap(),0,n);
        }
        parked=e->sh.parked;e->sh.parked=0;
        VaEngine**p=&g_shadow_chain;while(*p && *p!=e)p=&(*p)->next_shadow;if(*p)*p=e->next_shadow;
        __atomic_sub_fetch(&g_shadow_engines,1,__ATOMIC_ACQ_REL);
        va_unlock(k);
        va_log(line);
    }
    if(e->real){
        typedef void(VA_API*Unregister)(void*,void*);
        typedef VaU32(VA_API*Release)(void*);
        REAL(e,4,Unregister)(e->real,&e->callback_vt);
        REAL(e,2,Release)(e->real); /* Lost too: frees the parked real voices */
    }
    while(parked){VaParked*n=parked;parked=n->next;HeapFree(GetProcessHeap(),0,n);}
    if(e->sh.outage)__atomic_sub_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);
    va_report_pending_errors();VA_TRACE(e,"Release","-> %u",0UL);
    if(e->sh.rebuild)nt_stop(); /* 1.5 T4: after the teardown, outside g_cs */
    if(leak)return 0;
    while(e->sh.shims){VaShim*s=e->sh.shims;e->sh.shims=s->next;HeapFree(GetProcessHeap(),0,s);} /* real callbacks over */
    HeapFree(GetProcessHeap(),0,e);return 0;
}
static VaHR VA_API device_count(VaEngine*e,VaU32*out){
    if(!out)return VA_E_INVALIDARG;
    if(e->sh.outage){eng_lost(e);*out=1;return 0;} /* outage: one stereo device in every state */
    *out=e->real?1:0;return e->real?0:VA_E_FAIL;
}
static void wide_text(VaU16*out,const char*s){while(*s)*out++=(VaU16)*s++;*out=0;}
static VaHR VA_API device_details(VaEngine*e,VaU32 index,VaDevice*out){
    if(index || !out)return VA_E_INVALIDARG;
    if(e->sh.outage)eng_lost(e);else if(!e->real)return VA_E_FAIL; /* outage: stereo in every state */
    if(e->sh.outage){ /* 1.5 T2: the record the game first saw, in every state (a rebuild does not change it) */
        int k=va_lock(),have=k && e->sh.have_device;
        if(have){const VaU8*d=e->sh.device;volatile VaU8*o=(VaU8*)out;for(unsigned i=0;i<sizeof *out;++i)o[i]=d[i];}
        va_unlock(k);if(have){va_report_pending_errors();return 0;}
    }
    memset(out,0,sizeof *out);
    wide_text(out->device_id,"VinceAudio.Default");wide_text(out->display_name,"Windows default output (stereo)");
    out->role=15;out->format_tag=0xfffe;out->channels=2;out->rate=e->master_rate;
    out->bytes_per_second=out->rate*8;out->block_align=8;out->bits=32;out->extra_size=22;out->valid_bits=32;out->channel_mask=3;
    const VaGuid ieee_float={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    out->subformat=ieee_float;
    va_report_pending_errors();return 0;
}
typedef VaHR(VA_API*Create)(void**,VaU32,VaU32);
/* XAudio2Create from the 2.9 DLL (shared by Initialize and the 1.5 T1 rebuild); 0 = no module path (*out untouched). */
static int load_create(Create*out){
#ifdef VA_MOCK_BACKEND
    /* Offline harness build (build/mock, T6): the mock 2.9 engine beside this DLL. */
    VaU16 path[520];
    if(!module_file(path,"mock_xaudio2_9.dll"))return 0;
#else
    VaU16 path[520];VaU32 n=GetSystemDirectoryW(path,520);
    if(!n || n>480)return 0;
    path[n++]='\\';wide_text(path+n,"xaudio2_9.dll");
#endif
    /* Keep this module loaded until process exit; its worker threads own code. */
    void *module=LoadLibraryW(path);
#ifdef VA_MOCK_BACKEND
    if(module)g_mock_clock=(MockClockFn)GetProcAddress(module,"MockClock");
#endif
    *out=module?(Create)GetProcAddress(module,"XAudio2Create"):0;
    return 1;
}
static VaHR VA_API initialize(VaEngine*e,VaU32 flags,VaU32 processor){
    if(e->real)return VA_E_FAIL;
    e->init_flags=flags;e->init_processor=processor;e->have_init=1; /* 1.5 T1: a rebuild repeats this Initialize */
    if(ELOST(e))return 0;
    Create create=0;
    if(!load_create(&create))return VA_E_FAIL;
    e->create=create;
    if(!create){
        va_log("ERROR: system XAudio2 2.9 is unavailable.\r\n");
        va_sh_signal(&e->sh,VA_LOST_UNAVAILABLE,0,0);return eng_lost(e)?0:VA_E_FAIL; /* outage: Lost, silent engine */
    }
    VaHR hr=create(&e->real,flags,processor);
    if(hr<0){e->real=0;return eng_failed(e,"XAudio2Create",hr);}
    typedef VaHR(VA_API*Register)(void*,void*);
    e->callback_vt=callback_vt;
    hr=REAL(e,3,Register)(e->real,&e->callback_vt);
    if(hr<0){typedef VaU32(VA_API*Release)(void*);REAL(e,2,Release)(e->real);e->real=0;return eng_failed(e,"RegisterForCallbacks",hr);}
    va_log("XAudio2 2.9 adapter initialized: stereo, Windows default output, automatic device routing enabled.\r\n");
    return 0;
}
static VaHR VA_API register_callback(VaEngine*e,void*c){typedef VaHR(VA_API*F)(void*,void*);if(ELOST(e))return 0;return e->real?eng_failed(e,"RegisterForCallbacks",REAL(e,3,F)(e->real,c)):VA_E_FAIL;}
static void VA_API unregister_callback(VaEngine*e,void*c){typedef void(VA_API*F)(void*,void*);if(e->real && !ELOST(e))REAL(e,4,F)(e->real,c);}
static int has_effects(const void*effects){return effects && *(const VaU32*)effects;}
/* 1.5 T1, rebuild engine: a send whose target wrapper has real NULL (silent since an attempt). */
static int silent_target(VaEngine*e,const VaSends*sends,const VaSend*converted){
    if(!e->sh.rebuild || !sends)return 0;
    for(VaU32 i=0;i<sends->SendCount;++i)if(sends->pSends[i].pOutputVoice && !converted[i].pOutputVoice)return 1;
    return 0;
}

/* source/submix/master: *doomed = a created real voice the adapter could not wrap/register (t_* destroys it unlocked). */
static VaHR VA_API source(VaEngine*e,void**out,const void*fmt,VaU32 flags,float ratio,void*cb,const VaSends*sends,const void*effects,void**doomed){
    if(!out)return VA_E_INVALIDARG;*out=0;
    int lost=ELOST(e); /* Lost policy: no real call, a silent wrapper (real NULL) with its shadow record, S_OK */
    if(!e->real && !lost)return VA_E_FAIL;if(has_effects(effects))return VA_E_NOTIMPL;
    VaSend storage[64];VaSends converted;
    if(sends){VaHR hr=va_translate_sends(sends,&converted,storage,64);if(hr<0)return hr;}
    if(silent_target(e,sends,storage))lost=1; /* 1.5 T1: a send to a silent wrapper -> created silent, no real call */
    typedef VaHR(VA_API*F)(void*,void**,const void*,VaU32,float,void*,const VaSends*,const void*);
    /* 1.4 T4 D1: an outage source with a callback gets a shim (allocation failed: cb raw); switch off: cb raw. */
    VaShim *shim=e->sh.outage && cb?va_sh_shim_new(&e->sh,cb):0;
    void *real=0;VaHR hr=lost?0:REAL(e,5,F)(e->real,&real,fmt,flags,ratio,shim?(void*)shim:cb,sends?&converted:0,0);
    if(hr<0){if((hr=eng_failed(e,"CreateSourceVoice",hr))<0){if(shim)HeapFree(GetProcessHeap(),0,shim);return hr;}real=0;}int k=va_lock(),u=-1;*out=adopt(e,real,0,doomed);
    if(shim)va_sh_shim_link(&e->sh,shim,*out?((VaVoice*)*out)->rec:0); /* adopt failed: linked dead, freed at the final Release */
    if(*out && ((VaVoice*)*out)->rec)u=va_sh_source(((VaVoice*)*out)->rec,fmt,flags,ratio,cb,sends,effects);
    va_unlock(k);va_sh_note(u);return *out?hr:VA_E_OUTOFMEMORY;
}
static VaHR VA_API submix(VaEngine*e,void**out,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 stage,const VaSends*sends,const void*effects,void**doomed){
    if(!out)return VA_E_INVALIDARG;*out=0;
    int lost=ELOST(e); /* Lost policy: as source */
    if(!e->real && !lost)return VA_E_FAIL;if(has_effects(effects))return VA_E_NOTIMPL;
    VaSend storage[64];VaSends converted;
    if(sends){VaHR hr=va_translate_sends(sends,&converted,storage,64);if(hr<0)return hr;}
    if(silent_target(e,sends,storage))lost=1; /* 1.5 T1: as source */
    typedef VaHR(VA_API*F)(void*,void**,VaU32,VaU32,VaU32,VaU32,const VaSends*,const void*);
    void *real=0;VaHR hr=lost?0:REAL(e,6,F)(e->real,&real,channels,rate,flags,stage,sends?&converted:0,0);
    if(hr<0){if((hr=eng_failed(e,"CreateSubmixVoice",hr))<0)return hr;real=0;}int k=va_lock(),u=-1;*out=adopt(e,real,1,doomed);
    if(*out && ((VaVoice*)*out)->rec)u=va_sh_submix(((VaVoice*)*out)->rec,channels,rate,flags,stage,sends,effects);
    va_unlock(k);va_sh_note(u);return *out?hr:VA_E_OUTOFMEMORY;
}
static VaHR VA_API master(VaEngine*e,void**out,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 index,const void*effects,void**doomed){
    if(!out)return VA_E_INVALIDARG;*out=0;
    int lost=ELOST(e); /* Lost policy: as source; master_rate stays (48000 if never a live master) */
    if(!e->real && !lost)return VA_E_FAIL;
    if(index || (channels && channels!=2) || flags)return VA_E_INVALIDARG;
    if(has_effects(effects))return VA_E_NOTIMPL;
    typedef VaHR(VA_API*F)(void*,void**,VaU32,VaU32,VaU32,const VaU16*,const void*,VaU32);
    void *real=0;
    /* NULL device ID and flags=0 select the virtual default endpoint. Stereo
       keeps the game's existing matrices valid when physical outputs differ. */
    VaHR hr=lost?0:REAL(e,7,F)(e->real,&real,2,rate,0,0,0,6 /* AudioCategory_GameEffects */);
    if(hr<0){va_log("ERROR: default mastering voice creation failed.\r\n");if((hr=eng_failed(e,"CreateMasteringVoice",hr))<0)return hr;real=0;}
    typedef void(VA_API*Details)(void*,VaDetails29*);VaDetails29 d;
    if(real){((Details)(*(void***)real)[0])(real,&d);e->master_rate=e->sh.master_rate=d.InputSampleRate;}
    int k=va_lock();*out=adopt(e,real,2,doomed);
    if(*out && ((VaVoice*)*out)->rec)va_sh_master(((VaVoice*)*out)->rec,channels,rate,flags,index,effects);
    va_unlock(k);
    if(*out && real){ /* 3.1 T2: MasterVolume m on the real master only (game volume 1.0 at create) */
        va_master_game_vol=1.0f;
        if(va_master_volume!=1.0f){typedef VaHR(VA_API*Fl)(void*,float,VaU32);((Fl)(*(void***)real)[12])(real,va_master_volume,0);}
    }
    if(*out && real)va_log("Default mastering voice created.\r\n");
    return *out?hr:VA_E_OUTOFMEMORY;
}
static VaHR VA_API start(VaEngine*e){typedef VaHR(VA_API*F)(void*);if(ELOST(e))return 0;return e->real?eng_failed(e,"StartEngine",REAL(e,8,F)(e->real)):VA_E_FAIL;}
static void VA_API stop(VaEngine*e){typedef void(VA_API*F)(void*);if(e->real && !ELOST(e))REAL(e,9,F)(e->real);}
static VaHR VA_API commit(VaEngine*e,VaU32 set){typedef VaHR(VA_API*F)(void*,VaU32);if(ELOST(e))return 0;return e->real?eng_failed(e,"CommitChanges",REAL(e,10,F)(e->real,set)):VA_E_FAIL;}
static void VA_API performance(VaEngine*e,void*out){typedef void(VA_API*F)(void*,void*);if(e->real && out && !ELOST(e))REAL(e,11,F)(e->real,out);}
static void VA_API debug(VaEngine*e,const void*config,void*reserved){typedef void(VA_API*F)(void*,const void*,void*);if(e->real && !ELOST(e))REAL(e,12,F)(e->real,config,reserved);}
/* Unshadowed engine call on a shadow engine (T4 D7): counted after a successful call, first hit logged unlocked. */
static void sh_un(VaEngine*e,int method,VaHR hr){
    if(!e->shadow || hr<0)return;
    int k=va_lock(),u=k?va_sh_count(&e->sh,method):-1;va_unlock(k);va_sh_note(u);
}
/* Trace wrappers: one line after each call (adapter-side early errors included). Release traces inline. */
static VaHR VA_API t_query(VaEngine*e,const VaGuid*iid,void**out){VaHR hr=query(e,iid,out);VA_TRACE(e,"QueryInterface","iid=%p out=%p -> 0x%x",iid,out,hr);return hr;}
static VaU32 VA_API t_addref(VaEngine*e){VaU32 n=addref(e);VA_TRACE(e,"AddRef","-> %u",n);return n;}
static VaHR VA_API t_device_count(VaEngine*e,VaU32*out){VaHR hr=device_count(e,out);VA_TRACE(e,"GetDeviceCount","out=%p -> 0x%x",out,hr);return hr;}
static VaHR VA_API t_device_details(VaEngine*e,VaU32 index,VaDevice*out){VaHR hr=device_details(e,index,out);
    if(hr>=0 && e->shadow){int k=va_lock();if(k)va_sh_device(&e->sh,out);va_unlock(k);}VA_TRACE(e,"GetDeviceDetails","index=%u out=%p -> 0x%x",index,out,hr);return hr;}
static VaHR VA_API t_initialize(VaEngine*e,VaU32 flags,VaU32 processor){eng_in(e);VaHR hr=initialize(e,flags,processor);eng_out(e);VA_TRACE(e,"Initialize","flags=0x%x processor=0x%x -> 0x%x",flags,processor,hr);return hr;}
static VaHR VA_API t_register_callback(VaEngine*e,void*c){eng_in(e);VaHR hr=register_callback(e,c);eng_out(e);sh_un(e,VA_UN_RegisterForCallbacks,hr);VA_TRACE(e,"RegisterForCallbacks","callback=%p -> 0x%x",c,hr);return hr;}
static void VA_API t_unregister_callback(VaEngine*e,void*c){eng_in(e);unregister_callback(e,c);eng_out(e);if(e->real)sh_un(e,VA_UN_UnregisterForCallbacks,0);VA_TRACE(e,"UnregisterForCallbacks","callback=%p -> void",c);}
static VaHR VA_API t_source(VaEngine*e,void**out,const void*fmt,VaU32 flags,float ratio,void*cb,const VaSends*sends,const void*effects){
    eng_in(e);void *doomed=0;VaHR hr=source(e,out,fmt,flags,ratio,cb,sends,effects,&doomed);
    VA_TRACE(e,"CreateSourceVoice","voice=%p format=%p flags=0x%x ratio=%f callback=%p sends=%p effects=%p -> 0x%x",
             out?*out:0,fmt,flags,va_fbits(ratio),cb,sends,effects,hr);destroy_real(doomed);eng_out(e);return hr;
}
static VaHR VA_API t_submix(VaEngine*e,void**out,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 stage,const VaSends*sends,const void*effects){
    eng_in(e);void *doomed=0;VaHR hr=submix(e,out,channels,rate,flags,stage,sends,effects,&doomed);
    VA_TRACE(e,"CreateSubmixVoice","voice=%p channels=%u rate=%u flags=0x%x stage=%u sends=%p effects=%p -> 0x%x",
             out?*out:0,channels,rate,flags,stage,sends,effects,hr);destroy_real(doomed);eng_out(e);return hr;
}
static VaHR VA_API t_master(VaEngine*e,void**out,VaU32 channels,VaU32 rate,VaU32 flags,VaU32 index,const void*effects){
    eng_in(e);void *doomed=0;VaHR hr=master(e,out,channels,rate,flags,index,effects,&doomed);
    VA_TRACE(e,"CreateMasteringVoice","voice=%p channels=%u rate=%u flags=0x%x device=%u effects=%p -> 0x%x",
             out?*out:0,channels,rate,flags,index,effects,hr);destroy_real(doomed);eng_out(e);return hr;
}
static VaHR VA_API t_start(VaEngine*e){eng_in(e);VaHR hr=start(e);eng_out(e);sh_un(e,VA_UN_StartEngine,hr);VA_TRACE(e,"StartEngine","-> 0x%x",hr);return hr;}
static void VA_API t_stop(VaEngine*e){eng_in(e);stop(e);eng_out(e);if(e->real)sh_un(e,VA_UN_StopEngine,0);VA_TRACE(e,"StopEngine","-> void");}
static VaHR VA_API t_commit(VaEngine*e,VaU32 set){eng_in(e);VaHR hr=commit(e,set);eng_out(e);sh_un(e,VA_UN_CommitChanges,hr);VA_TRACE(e,"CommitChanges","operationSet=%u -> 0x%x",set,hr);return hr;}
static void VA_API t_performance(VaEngine*e,void*out){eng_in(e);performance(e,out);eng_out(e);VA_TRACE(e,"GetPerformanceData","out=%p -> void",out);}
static void VA_API t_debug(VaEngine*e,const void*config,void*reserved){eng_in(e);debug(e,config,reserved);eng_out(e);VA_TRACE(e,"SetDebugConfiguration","config=%p reserved=%p -> void",config,reserved);}
static void *engine_vt[]={(void*)t_query,(void*)t_addref,(void*)release,(void*)t_device_count,(void*)t_device_details,(void*)t_initialize,
    (void*)t_register_callback,(void*)t_unregister_callback,(void*)t_source,(void*)t_submix,(void*)t_master,(void*)t_start,(void*)t_stop,
    (void*)t_commit,(void*)t_performance,(void*)t_debug};

/* ---- Rebuild (1.5 T1, VinceAudio.rebuild): a Lost engine recreates its real engine, master and (1.5 T2) voice graph on
   a game thread. One claimant (rb_busy); throttle 0, +250, +500, +1000, then +2000 ms forever (va_clock). In-flight real
   calls are drained first (sh.calls <= self, ~20 ms, else the attempt is dropped unthrottled). Under g_cs every real is
   detached (wrappers silent, every record held: outage clock ticked once, then frozen), released unlocked, then the
   engine and master are created again with the Initialize arguments; rb_graph recreates every other node stopped from
   its shadow and flips all the new reals under g_cs, clearing lost last; a failure leaves the wrappers silent.
   2.2 T2: the release of the old reals and the engine/master creation (rb_job) run on a per-engine worker thread (created
   at the engine's first attempt); the game waits <= 8 ms for its result, else returns Lost and a later entry collects it
   (detach, rb_graph, flip and every log line stay on game threads). VinceAudio.syncrebuild runs rb_job inline (2.1). ---- */
#define RB(o,i,T) ((T)(*(void***)(o))[i])
/* 1.5 T2 graph job: one node's creation and/or re-apply, built under g_cs (rb_build), run unlocked (rb_run). */
typedef struct {
    VaU32 kind,create,flags,channels,rate,stage,nsends,has_sends,mats,q,sub,ret,from,dest_ok[2],drop; /* drop: 1.5 T3 */
    float max_ratio,vol,ratio; void *v,*cb,*dest[2]; VaU8 fmt[18+64]; VaSend send[4];
    VaShadowMatrix mat[2]; VaBuffer29 buf[64];
} RbJob;
typedef struct { void *real,*w; VaU32 flags; } RbStart;
static void rb_copy(void*d,const void*s,unsigned n){volatile VaU8*o=d;const VaU8*i=s;while(n--)*o++=*i++;}
static void rb_free(VaParked*p){while(p){VaParked*n=p;p=n->next;destroy_real(n->real);HeapFree(GetProcessHeap(),0,n);}}
/* g_cs: a send/matrix target's new real. 1 = *out (target NULL: NULL), 0 = not made yet, -1 = never (silent, gone). */
static int rb_target(VaEngine*e,VaShadowNode*mn,void*m,void*w,void**out){
    *out=0;if(!w)return 1;
    for(VaShadowNode*n=e->nodes;n;n=n->next)if(n->wrapper==w){
        if(n->kind==2){if(n!=mn || !m)return -1;*out=m;return 1;}
        if(n->rb_real){*out=n->rb_real;return 1;}
        return n->rb_skip?-1:0;
    }
    return -1;
}
/* g_cs: the node has work: not made yet, or its shadow moved from the applied snapshot (the master: its volume). */
static int rb_dirty(VaShadowNode*n,VaShadowNode*mn){
    if(n->rb_skip)return 0;
    const VaShadowVoice*s=va_sh_voice(n->rec);
    if(n->kind==2)return n==mn && (!n->rb_done || n->rb_vol!=s->volume);
    if(!n->rb_real || !n->rb_done)return 1;
    return n->rb_vol!=s->volume || n->rb_ratio!=s->ratio || n->rb_mats!=s->matrices
        || !equal(n->rb_mat,s->matrix,s->matrices*sizeof s->matrix[0]) || n->rb_q!=s->queued || n->rb_sub!=s->submitted || n->rb_ret!=s->retired;
}
/* g_cs: 1 = job ready, 0 = a target not made yet (next round), 2 = kept silent (a target never comes), -1 = the queue is not
   a pure append since the last apply (Flush/retire during the attempt). Formats: 18-byte WAVEFORMATEX + cbSize (<= 64). */
static int rb_build(VaEngine*e,VaShadowNode*n,VaShadowNode*mn,void*m,RbJob*j){
    const VaShadowVoice*s=va_sh_voice(n->rec);
    j->kind=n->kind;j->create=n->kind!=2 && !n->rb_real;j->v=n->kind==2?m:n->rb_real;
    j->vol=s->volume;j->ratio=s->ratio;j->q=s->queued;j->sub=s->submitted;j->ret=s->retired;j->mats=0;j->from=0;j->drop=0;
    if(n->kind==2){j->q=0;return 1;}
    if(j->create){
        j->flags=s->flags;j->channels=s->channels;j->rate=s->rate;j->stage=s->stage;j->max_ratio=s->max_ratio;
        j->has_sends=s->sends!=0;j->nsends=s->send_count<4?s->send_count:4;
        for(VaU32 i=0;i<j->nsends;++i){
            int t=rb_target(e,mn,m,s->send[i].pOutputVoice,&j->send[i].pOutputVoice);
            if(t<0){n->rb_skip=1;return 2;}
            if(!t)return 0;
            j->send[i].Flags=s->send[i].Flags;
        }
        if(!n->kind){
            const VaShadowFormat*f=&s->format;VaU16 cb=f->tag==1?0:f->cb_size>64?64:f->cb_size;VaU8*o=j->fmt;
            rb_copy(o,&f->tag,2);rb_copy(o+2,&f->channels,2);rb_copy(o+4,&f->rate,4);rb_copy(o+8,&f->avg_bytes,4);
            rb_copy(o+12,&f->align,2);rb_copy(o+14,&f->bits,2);rb_copy(o+16,&cb,2);rb_copy(o+18,f->extra,cb);
            j->cb=va_sh_callback(n->rec);
        }
    }
    else{
        if(s->retired!=n->rb_ret || s->queued<n->rb_q || s->queued-n->rb_q!=s->submitted-n->rb_sub)return -1;
        j->from=n->rb_q;
    }
    for(VaU32 i=0;i<s->matrices && i<2;++i){
        rb_copy(&j->mat[i],&s->matrix[i],sizeof j->mat[i]);
        int t=rb_target(e,mn,m,s->matrix[i].dest,&j->dest[i]);
        if(!t)return 0;
        j->dest_ok[i]=t>0;
    }
    j->mats=s->matrices;
    int src=j->create && !n->kind,keeps=src && va_sh_keeps(n->rec);
    if(src && !keeps){j->from=j->q;j->drop=j->q;} /* 1.5 T3: no callback, no loop: nothing resubmitted, retired at the flip */
    for(VaU32 i=j->from;i<s->queued;++i)rb_copy(&j->buf[i-j->from],&s->queue[i],sizeof j->buf[0]);
    if(keeps && j->q)va_sh_resume(n->rec,&j->buf[0]); /* 1.5 T3: the head resumes at the outage clock's pos */
    return 1;
}
/* Unlocked, on the stopped new voice: the creation hr (j->v NULL on failure), else the first fatal hr of the applies, else 0. */
static VaHR rb_run(void*real,RbJob*j){
    typedef VaHR(VA_API*Src)(void*,void**,const void*,VaU32,float,void*,const VaSends*,const void*);
    typedef VaHR(VA_API*Mix)(void*,void**,VaU32,VaU32,VaU32,VaU32,const VaSends*,const void*);
    typedef VaHR(VA_API*Fl)(void*,float,VaU32);typedef VaHR(VA_API*Mat)(void*,void*,VaU32,VaU32,const float*,VaU32);
    typedef VaHR(VA_API*Sub)(void*,const void*,const void*);
    if(j->create){
        VaSends sl;sl.SendCount=j->nsends;sl.pSends=j->send;void*v=0;
        VaHR hr=!j->kind?RB(real,5,Src)(real,&v,j->fmt,j->flags,j->max_ratio,j->cb,j->has_sends?&sl:0,0)
                       :RB(real,6,Mix)(real,&v,j->channels,j->rate,j->flags,j->stage,j->has_sends?&sl:0,0);
        if(hr<0 || !v){j->v=0;return hr<0?hr:VA_E_FAIL;}
        j->v=v;
    }
    void*v=j->v;VaHR bad=0;if(!v)return 0;
#define RB_HR(x) do{VaHR h_=(x);if(!bad && va_sh_fatal(h_))bad=h_;}while(0)
    RB_HR(RB(v,12,Fl)(v,j->kind==2?j->vol*va_master_volume:j->vol,0)); /* 3.1 T2: m on the real master only */
    if(!j->kind)RB_HR(RB(v,26,Fl)(v,j->ratio,0));
    for(VaU32 i=0;i<j->mats && i<2;++i)if(j->dest_ok[i])RB_HR(RB(v,16,Mat)(v,j->dest[i],j->mat[i].src,j->mat[i].dst,j->mat[i].m,0));
    for(VaU32 i=j->from;i<j->q;++i)RB_HR(RB(v,21,Sub)(v,&j->buf[i-j->from],0));
#undef RB_HR
    return bad;
}
/* Unlocked: an attempt that does not go Live: its new voices destroyed (reverse creation order), holds cleared. */
static void rb_cleanup(VaEngine*e){
    for(;;){
        int k=va_lock();VaShadowNode*last=0;void*v=0;VaParked*gone=e->sh.rb_gone;e->sh.rb_gone=0;e->sh.rb_job=0;
        for(VaShadowNode*n=e->nodes;n;n=n->next){va_sh_unhold(n->rec);if(n->rb_real)last=n;}
        if(last){v=last->rb_real;last->rb_real=0;}
        va_unlock(k);
        rb_free(gone);
        if(!v)return;
        destroy_real(v);
    }
}
/* 1.5 T2: reconcile the new graph against the game's calls on the silent wrappers (<= 4 rounds): under g_cs pick the next
   node with work (master volume, submixes, sources; creation order), run its job unlocked, store the snapshot; none left ->
   flip in that locked section once no wrapper path is in flight (else a silent-path Submit misses its new real; <= 50 ms,
   else dropped) and the shims' rings are drained (<= 50 ms, else dropped: 1.5 T3, outage deliveries stop at the swap).
   After the flip, unlocked: Start each started source, then Stop it if the game stopped it meanwhile. */
static int rb_graph(VaEngine*e,void*real,void*m,VaShadowNode*mn,int self,VaHR*out,const char**step){
    typedef VaHR(VA_API*Go)(void*,VaU32,VaU32);
    VaShadowEng*s=&e->sh;RbJob j;VaU32 round=1,waits=0;
    for(;;){
        int k=va_lock();
        if(__atomic_load_n(&s->claim,__ATOMIC_SEQ_CST)){va_unlock(k);return 3;}
        VaShadowNode*pick=0;int any=0,bad=0;
        for(unsigned pass=0;pass<3 && !pick && !bad;++pass)
            for(VaShadowNode*n=e->nodes;n;n=n->next){
                if(n->kind!=2-pass)continue;
                va_sh_hold(n->rec); /* nodes made during the attempt too */
                if(!rb_dirty(n,mn))continue;
                any=1;if(n->rb_round==round)continue;
                n->rb_round=round;
                int b=rb_build(e,n,mn,m,&j);
                if(b==1){pick=n;break;}
                if(b<0){bad=1;break;}
            }
        if(bad){va_unlock(k);return 1;}
        if(pick){
            s->rb_job=pick;va_unlock(k);
            VaHR hr=rb_run(real,&j);int fatal=va_sh_fatal(hr),silent=0;void*orphan=0;
            k=va_lock();
            VaShadowNode*n=s->rb_job;s->rb_job=0;
            if(!n){if(j.create)orphan=j.v;} /* destroyed meanwhile */
            else if(j.create && !j.v){if(!fatal){n->rb_skip=1;silent=1;}}
            else{
                if(j.create){n->rb_real=j.v;n->rb_drop=j.drop;} /* 1.5 T3 */
                n->rb_done=1;n->rb_vol=j.vol;n->rb_ratio=j.ratio;n->rb_mats=j.mats;rb_copy(n->rb_mat,j.mat,sizeof n->rb_mat);
                n->rb_q=j.q;n->rb_sub=j.sub;n->rb_ret=j.ret;
            }
            VaParked*gone=s->rb_gone;s->rb_gone=0;
            va_unlock(k);
            destroy_real(orphan);rb_free(gone);
            if(silent){char line[64];if(va_format(line,sizeof line,"rebuild: voice kept silent 0x%x\r\n",(VaU32)hr))va_log(line);}
            if(fatal){*out=hr;*step=j.create && !j.v?"voice":"graph";return 2;} /* 1.6 T1: which call failed */
            continue;
        }
        if(any){va_unlock(k);if(++round>4)return 1;continue;}
        int drained=1,quiet=__atomic_load_n(&s->calls,__ATOMIC_SEQ_CST)<=self;
        for(VaShim*x=s->shims;x && drained;x=x->next)
            if(!x->dead && (x->ev_n || __atomic_load_n(&x->busy,__ATOMIC_SEQ_CST)))drained=0;
        if(!quiet || !drained){
            va_unlock(k);
            if(waits>=50)return 1; /* 1.5 T3: never flip with a ring left */
            ++waits;Sleep(1);continue;
        }
        VaU32 cnt=0,ng=0;
        for(VaShadowNode*n=e->nodes;n;n=n->next)if(n->rb_real && !n->kind)++cnt;
        RbStart*go=cnt?HeapAlloc(GetProcessHeap(),0,cnt*sizeof *go):0;
        if(cnt && !go){va_unlock(k);return 1;}
        e->real=real;void*orphan=0;
        VaShadowNode*x=e->nodes;while(x && x!=mn)x=x->next;
        if(m && x && x->kind==2){((VaVoice*)x->wrapper)->real=m;__atomic_sub_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);}
        else orphan=m; /* the master wrapper went during the attempt */
        for(VaShadowNode*n=e->nodes;n;n=n->next){
            if(!n->rb_real){va_sh_unhold(n->rec);continue;} /* kept silent */
            VaVoice*w=n->wrapper;w->real=n->rb_real;__atomic_sub_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);
            va_sh_attach(n->rec,n->rb_drop); /* 1.5 T3 */
            const VaShadowVoice*v=va_sh_voice(n->rec);
            if(!n->kind && v->started){go[ng].real=n->rb_real;go[ng].w=w;go[ng].flags=v->start_flags;++ng;}
            n->rb_real=0;
        }
        s->rb_n=0;s->rb_delay_ms=0;s->rb_last_hr=0;s->rb_started=0;s->rb_next=0;if(ng)__atomic_store_n(&s->rb_go,1,__ATOMIC_SEQ_CST);__atomic_store_n(&s->lost,0,__ATOMIC_SEQ_CST);
        VaParked*gone=s->rb_gone;s->rb_gone=0; /* 1.5 T3: nodes destroyed after their job ran (drain wait): their new reals */
        va_unlock(k);
        destroy_real(orphan);rb_free(gone);
        for(VaU32 i=0;i<ng;++i){
            VaHR hr=RB(go[i].real,19,Go)(go[i].real,go[i].flags,0);
            k=va_lock();VaShadowRec*r=va_shadow_find(go[i].w);VaU32 stop=0,flags=0;
            if(r && ((VaVoice*)go[i].w)->real==go[i].real && !va_sh_voice(r)->started){stop=1;flags=va_sh_voice(r)->stop_flags;}
            va_unlock(k);
            if(stop)RB(go[i].real,20,Go)(go[i].real,flags,0);
            if(va_sh_fatal(hr)){va_sh_signal(s,VA_LOST_FAILED,"Start",hr);*step="start";}
        }
        __atomic_store_n(&s->rb_go,0,__ATOMIC_RELEASE); /* a Live DestroyVoice of a started voice waited for this */
        if(go)HeapFree(GetProcessHeap(),0,go);
        return 0;
    }
}
/* 1.6 T1: tick span a..b in ms (0 when b is unset or earlier) */
static unsigned long long rb_ms(unsigned long long a,unsigned long long b,unsigned long long f){return b>a?udiv64((b-a)*1000,f,0):0;}
/* 1.6 T1: + step (0 on Live) and took (ms from now); took >= 100 logs even a repeated hr, with the step times
   t[0..5] = start, released, created, registered, mastered, graphed (ticks; 0 = not reached).
   2.2 T1: + game (ms from g0, the game thread's start tick; today the whole attempt, T2 moves g0).
   2.2 T2: wk (worker path): + worker (ms t[0]..s->wk_pub) before game; game = gpre (the start call's ticks) + g0..now. */
static void rb_log_attempt(VaShadowEng*s,VaU32 n,VaHR hr,VaU32 last,unsigned long long now,unsigned long long f,
                           const char*step,const unsigned long long*t,unsigned long long g0,int wk,unsigned long long gpre){
    unsigned long long took=rb_ms(now,va_clock(0),f);
    if(n!=1 && (VaU32)hr==last && took<100)return; /* only when the hr changes (attempt 1 always) */
    char line[256];unsigned long long ms=now>s->lost_at?udiv64((now-s->lost_at)*1000,f,0):0;
    unsigned k=va_format(line,sizeof line,"rebuild: attempt %u 0x%x %Q",n,(VaU32)hr,ms);
    if(step)k+=va_format(line+k,sizeof line-k," step=%s",step);
    k+=va_format(line+k,sizeof line-k," took=%Q",took);
    if(took>=100){
        unsigned long long d[5];for(int i=0;i<5;++i)d[i]=t[i+1]?rb_ms(t[i],t[i+1],f):0;
        k+=va_format(line+k,sizeof line-k," release=%Q create=%Q register=%Q master=%Q graph=%Q",d[0],d[1],d[2],d[3],d[4]);
    }
    if(wk){
        unsigned long long c=va_clock(0),g=(c>g0?c-g0:0)+gpre;
        k+=va_format(line+k,sizeof line-k," worker=%Q game=%Q",rb_ms(t[0],s->wk_pub,f),udiv64(g*1000,f,0));
    }
    else k+=va_format(line+k,sizeof line-k," game=%Q",rb_ms(g0,va_clock(0),f));
    if(va_format(line+k,sizeof line-k,"\r\n"))va_log(line);
}
/* 2.2 T2: the attempt's job on its wk_* copies (worker thread, or inline on the sync path): the old reals released (parked,
   voices, master, engine), claim/reason cleared, the engine and master created again; outputs in wk_*, wk_pub = its end.
   No wrapper, node, shim, g_cs or log. */
static void rb_job(VaEngine*e){
    VaShadowEng*s=&e->sh;
    typedef void(VA_API*Unregister)(void*,void*);typedef VaU32(VA_API*Release)(void*);typedef VaHR(VA_API*Register)(void*,void*);
    typedef VaHR(VA_API*Master)(void*,void**,VaU32,VaU32,VaU32,const VaU16*,const void*,VaU32);
    unsigned long long*t=s->wk_t;for(int i=0;i<6;++i)t[i]=0;const char*step=0;t[0]=va_clock(0); /* 1.6 T1: step times, rb_log_attempt */
    VaParked*parked=s->wk_parked,*voices=s->wk_voices;void*old=s->wk_old,*mreal=s->wk_mreal;
    s->wk_parked=s->wk_voices=0;s->wk_old=s->wk_mreal=0;
    while(parked){VaParked*p=parked;parked=p->next;destroy_real(p->real);HeapFree(GetProcessHeap(),0,p);}
    while(voices){VaParked*p=voices;voices=p->next;destroy_real(p->real);HeapFree(GetProcessHeap(),0,p);}
    destroy_real(mreal);
    if(old){RB(old,4,Unregister)(old,&e->callback_vt);RB(old,2,Release)(old);}
    __atomic_store_n(&s->reason,0,__ATOMIC_RELEASE);__atomic_store_n(&s->claim,0,__ATOMIC_SEQ_CST); /* a new loss can claim */
    t[1]=va_clock(0);
    Create create=(Create)s->wk_create;
    void*real=0,*m=0;VaHR hr=VA_E_FAIL;step="create";
    if(create){
        hr=create(&real,s->wk_flags,s->wk_proc);t[2]=va_clock(0);
        if(hr<0)real=0;
        else{
            hr=RB(real,3,Register)(real,&e->callback_vt);t[3]=t[4]=va_clock(0);step="register";
            if(hr>=0 && s->wk_has_master){
                hr=RB(real,7,Master)(real,&m,2,s->wk_rate,0,0,0,6 /* AudioCategory_GameEffects, as master() */);t[4]=va_clock(0);step="master";
                if(hr<0){m=0;RB(real,4,Unregister)(real,&e->callback_vt);}
            }
            if(hr<0){RB(real,2,Release)(real);real=0;}
        }
    }
    VaU32 live_rate=0,live_ch=0; /* 1.6 T1: rebuild: Live */
    if(hr>=0 && m){typedef void(VA_API*Details)(void*,VaDetails29*);VaDetails29 d;RB(m,0,Details)(m,&d);
        live_rate=d.InputSampleRate;live_ch=d.InputChannels;}
    s->wk_real=real;s->wk_m=m;s->wk_hr=hr;s->wk_step=step;s->wk_live_rate=live_rate;s->wk_live_ch=live_ch;
    s->wk_pub=va_clock(0);
}
/* 2.2 T2: the per-engine worker: each wake with rb_busy 3 runs rb_job and publishes it (3->4); a failed publish (Release
   abandoned the job, 3->2, e leaked) discards the new engine and master and exits; wk_quit exits. */
static VaU32 VA_API rb_worker(void*arg){
    VaEngine*e=arg;VaShadowEng*s=&e->sh;
#ifndef VA_MOCK_BACKEND
    VaHR init=CoInitializeEx(0,0); /* COINIT_MULTITHREADED, as nt_thread */
#endif
    for(;;){
        WaitForSingleObject(s->wk_wake,0xFFFFFFFFUL);
        if(__atomic_load_n(&s->wk_quit,__ATOMIC_SEQ_CST))break;
        if(__atomic_load_n(&s->rb_busy,__ATOMIC_ACQUIRE)!=3)continue;
        rb_job(e);
        long z=3;
        if(!__atomic_compare_exchange_n(&s->rb_busy,&z,4,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
            typedef void(VA_API*Unregister)(void*,void*);typedef VaU32(VA_API*Release)(void*);
            void*r=s->wk_real;destroy_real(s->wk_m);s->wk_real=s->wk_m=0;
            if(r){RB(r,4,Unregister)(r,&e->callback_vt);RB(r,2,Release)(r);}
            break;
        }
    }
#ifndef VA_MOCK_BACKEND
    if(init==0 || init==1)CoUninitialize(); /* S_OK / S_FALSE only */
#endif
    return 0;
}
/* 2.2 T2: the attempt's collect half on a game thread (rb_busy 1): the graph and flip, the log lines, the throttle step.
   g0 = this call's start tick, gpre = ticks the attempt's start call spent (worker path), wk = worker path (log fields). */
static void rb_collect(VaEngine*e,int self,unsigned long long g0,unsigned long long gpre,int wk){
    VaShadowEng*s=&e->sh;
    typedef void(VA_API*Unregister)(void*,void*);typedef VaU32(VA_API*Release)(void*);
    void*real=s->wk_real,*m=s->wk_m;VaHR hr=s->wk_hr;const char*step=s->wk_step;s->wk_real=s->wk_m=0;
    VaU32 n=s->wk_n,last=s->wk_last;unsigned long long now=s->wk_now,f=s->wk_f,t[6];for(int i=0;i<6;++i)t[i]=s->wk_t[i];
    VaU32 live_rate=s->wk_live_rate,live_ch=s->wk_live_ch; /* 1.6 T1: rebuild: Live */
    if(hr>=0 && m)e->master_rate=s->master_rate=live_rate;
    int lostagain=hr>=0 && __atomic_load_n(&s->claim,__ATOMIC_SEQ_CST); /* a loss while Rebuilding */
    /* 1.5 T2: the voice graph; 0 = Live (flipped under g_cs, lost cleared), 1 = dropped, 2 = failed (hr), 3 = a loss meanwhile */
    if(hr>=0)step="graph";
    int k=va_lock();VaShadowNode*mn=s->wk_mn;va_unlock(k); /* 2.2 T2: 0 when the master node went meanwhile */
    int st=hr>=0 && !lostagain?rb_graph(e,real,m,mn,self,&hr,&step):-1;
    if(st>=0)t[5]=va_clock(0);
    if(!st){
        rb_log_attempt(s,n,0,last,now,f,0,t,g0,wk,gpre);
        char line[96];if(va_format(line,sizeof line,"rebuild: Live rate=%u ch=%u\r\n",live_rate,live_ch))va_log(line);
        nt_live(s); /* 1.6 T2; with 1.6 T1's g_nt_req (endpoint lines) */
        __atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;
    }
    rb_cleanup(e); /* not Live: the new voices go, holds cleared, the wrappers stay silent */
    if(st==3)lostagain=1;
    if(st==1 || st==2){ /* then the new master, then the engine */
        destroy_real(m);RB(real,4,Unregister)(real,&e->callback_vt);RB(real,2,Release)(real);real=m=0;
        if(st==1){--s->rb_n;__atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;} /* dropped: no throttle step */
    }
    void*orphan=0;
    if(lostagain){
        k=va_lock();
        e->real=real;mn=s->wk_mn;
        VaShadowNode*x=e->nodes;while(x && x!=mn)x=x->next;
        if(m && x && x->kind==2){((VaVoice*)x->wrapper)->real=m;__atomic_sub_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);}
        else orphan=m; /* the master wrapper went during the attempt */
        va_unlock(k);
        destroy_real(orphan);
    }
    if(lostagain){ /* kept as the old objects for the next attempt; the attempt failed with the new loss's hr */
        char line[128],why[96];va_sh_reason(s,why,sizeof why);
        if(va_format(line,sizeof line,"outage: Lost (%s)\r\n",why))va_log(line);
        hr=(VaHR)s->hr;step="lost";
    }
    rb_log_attempt(s,n,hr,last,now,f,step,t,g0,wk,gpre);s->rb_last_hr=(VaU32)hr;
    /* 1.5 T4: no default render endpoint known -> the 2000 ms cap at once (an appearance resets it, above) */
    s->rb_delay_ms=__atomic_load_n(&g_nt_present,__ATOMIC_ACQUIRE)==0?2000:!s->rb_delay_ms?250:s->rb_delay_ms>=1000?2000:s->rb_delay_ms*2;
    s->rb_next=now+udiv64(f*s->rb_delay_ms,1000,0);
    __atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);
}
void va_rebuild(VaShadowEng*s,int self){
    VaEngine*e=(VaEngine*)((VaU8*)s-__builtin_offsetof(VaEngine,sh));
    if(self<0 || !s->rebuild || GetCurrentThreadId()==s->thread_id)return; /* never on the outage thread */
    long z=0;
    if(!__atomic_compare_exchange_n(&s->rb_busy,&z,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
        if(z==4 && __atomic_compare_exchange_n(&s->rb_busy,&z,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) /* 2.2 T2: a published result */
            rb_collect(e,self,va_clock(0),__atomic_load_n(&s->wk_gpre,__ATOMIC_ACQUIRE),1);
        return; /* 1 or 2; 3: the worker's job in flight (skipped) */
    }
    unsigned long long f=1,now=va_clock(&f);if(!f)f=1;
    int go=__atomic_load_n(&s->lost,__ATOMIC_SEQ_CST) && e->have_init;
    if(go){ /* 1.5 T4: this attempt acts on the notifier's generation; a device appearance skips the throttle */
        __atomic_store_n(&s->nt_seen,__atomic_load_n(&g_nt_gen,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
        VaU32 ap=(VaU32)__atomic_load_n(&g_nt_appear,__ATOMIC_ACQUIRE);
        if(ap!=s->nt_appear_seen){s->nt_appear_seen=ap;s->rb_next=0;s->rb_delay_ms=0;}
    }
    if(!go || (s->rb_n && now<s->rb_next)){
        __atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;
    }
    for(int i=0;i<20 && __atomic_load_n(&s->calls,__ATOMIC_SEQ_CST)>self;++i)Sleep(1);
    if(__atomic_load_n(&s->calls,__ATOMIC_SEQ_CST)>self){__atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;}
    /* Detach under g_cs: parked, then voices (reverse creation order), the master, the engine. */
    int k=va_lock(),busy=0;
    for(VaShim*x=s->shims;x && !busy;x=x->next)if(__atomic_load_n(&x->real_busy,__ATOMIC_SEQ_CST))busy=1;
    if(busy){va_unlock(k);__atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;}
    /* 1.5 T3: no attempt (not counted) while a ring still has deliveries; then every clock ticked once and frozen (what
       that tick pushes is drained by rb_graph before the flip, else the attempt is dropped) */
    for(VaShim*x=s->shims;x && !busy;x=x->next)
        if(!x->dead && (x->ev_n || __atomic_load_n(&x->busy,__ATOMIC_SEQ_CST)))busy=1;
    for(VaShadowNode*n=e->nodes;n;n=n->next)va_sh_hold(n->rec);
    if(busy){
        for(VaShadowNode*n=e->nodes;n;n=n->next)va_sh_unhold(n->rec);
        va_unlock(k);__atomic_store_n(&s->rb_busy,0,__ATOMIC_RELEASE);return;
    }
    VaParked*parked=s->parked,*voices=0;s->parked=0;
    VaShadowNode*mn=0;void*mreal=0;VaU32 rate=0;
    for(VaShadowNode*n=e->nodes;n;n=n->next){
        VaVoice*w=n->wrapper;
        n->rb_real=0;n->rb_skip=n->kind==2 && mn;n->rb_round=0;n->rb_done=0;n->rb_drop=0; /* rb_drop: 1.5 T3 */ /* 1.5 T2: a second master stays silent */
        va_sh_hold(n->rec); /* 1.5 T2: outage clock ticked once, then frozen until the flip or a failure */
        if(n->kind==2 && !mn){
            mn=n;rate=va_sh_rate(n->rec);
            if(w->real){mreal=w->real;w->real=0;__atomic_add_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);}
            continue;
        }
        if(w->real){
            VaParked*p=HeapAlloc(GetProcessHeap(),0,sizeof *p); /* none: the engine Release frees that voice */
            if(p){p->real=w->real;p->next=voices;voices=p;}
            w->real=0;__atomic_add_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);
        }
        va_sh_silence(n->rec);
    }
    void*old=e->real;e->real=0;s->wk_mn=mn;
    int first=!s->rb_started;s->rb_started=1;VaU32 n=++s->rb_n,last=s->rb_last_hr;
    char why[96];va_sh_reason(s,why,sizeof why);
    va_unlock(k);
    if(first){char line[128];if(va_format(line,sizeof line,"rebuild: start (%s)\r\n",why))va_log(line);}
    /* 2.2 T2: the job's inputs, set before the hand-off (load_create stays on the game thread) */
    if(!e->create){Create c=0;if(load_create(&c))e->create=c;}
    s->wk_old=old;s->wk_parked=parked;s->wk_voices=voices;s->wk_mreal=mreal;s->wk_has_master=mn!=0;s->wk_rate=rate;
    s->wk_create=(void*)e->create;s->wk_flags=e->init_flags;s->wk_proc=e->init_processor;
    s->wk_n=n;s->wk_last=last;s->wk_now=now;s->wk_f=f;__atomic_store_n(&s->wk_gpre,0,__ATOMIC_RELEASE);
    e->callback_vt=callback_vt;
    int wk=0;
    if(!s->syncrebuild){ /* the worker, created at this engine's first attempt; none -> the job runs inline */
        if(!s->wk_thread){
            s->wk_wake=CreateEventW(0,0,0,0); /* auto-reset */
            if(s->wk_wake)s->wk_thread=CreateThread(0,0,rb_worker,e,0,0);
            if(!s->wk_thread && s->wk_wake){CloseHandle(s->wk_wake);s->wk_wake=0;}
        }
        wk=s->wk_thread!=0;
    }
    if(!wk){rb_job(e);rb_collect(e,self,now,0,0);return;}
    z=1;__atomic_compare_exchange_n(&s->rb_busy,&z,3,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED); /* the job is the worker's */
    SetEvent(s->wk_wake);
    long long q0=0,q=0,qf=0;QueryPerformanceFrequency(&qf);if(qf<=0)qf=1;QueryPerformanceCounter(&q0);
    while(__atomic_load_n(&s->rb_busy,__ATOMIC_ACQUIRE)!=4){ /* <= 8 ms real time for the result */
        __atomic_store_n(&s->wk_gpre,va_clock(0)-now,__ATOMIC_RELEASE); /* this call's span, for a later collect */
        QueryPerformanceCounter(&q);
        if((unsigned long long)(q-q0)>=udiv64((unsigned long long)qf*8,1000,0))break;
        Sleep(0);
    }
    __atomic_store_n(&s->wk_gpre,va_clock(0)-now,__ATOMIC_RELEASE);
    z=4;
    if(__atomic_compare_exchange_n(&s->rb_busy,&z,1,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))rb_collect(e,self,now,0,1);
    /* else still Lost: a later entry collects the result (4->1) */
}
VaHR VA_API VinceAudioCreate(void**out){
    if(!out)return VA_E_INVALIDARG;*out=0;
    VaU16 path[520];
    /* 2.1 T1: off-files, read at every create; no file = shadow (1.3 T1), outage (1.4 T2) and rebuild (1.5 T1) all on.
       VinceAudio.norebuild: rebuild off; .nooutage: outage and rebuild off; .noshadow: all off (plain engine). */
    int noshadow=module_file(path,"VinceAudio.noshadow") && GetFileAttributesW(path)!=0xFFFFFFFFUL;
    int nooutage=noshadow || (module_file(path,"VinceAudio.nooutage") && GetFileAttributesW(path)!=0xFFFFFFFFUL);
    int norebuild=nooutage || (module_file(path,"VinceAudio.norebuild") && GetFileAttributesW(path)!=0xFFFFFFFFUL);
    /* 2.2 T2: VinceAudio.syncrebuild: the rebuild job on the game thread (no worker layer); only with rebuild on */
    int syncrebuild=!norebuild && module_file(path,"VinceAudio.syncrebuild") && GetFileAttributesW(path)!=0xFFFFFFFFUL;
#ifdef VINCE_AIO
    const char*mvline=va_mvline; /* read once at start-up (va_read_ini) */
#else
    char mvline[160];master_volume_read(mvline,sizeof mvline); /* 3.1 T1 */
#endif
    /* 2.1 T2: the first line of every create, before trace_switch (so before TraceStart) and the legacy lines. */
    static const char*const version_line[5]={"VinceAudio " VA_VERSION_STR " layers: none\r\n","VinceAudio " VA_VERSION_STR " layers: shadow\r\n",
        "VinceAudio " VA_VERSION_STR " layers: shadow outage\r\n","VinceAudio " VA_VERSION_STR " layers: shadow outage rebuild\r\n",
        "VinceAudio " VA_VERSION_STR " layers: shadow outage rebuild worker\r\n"};
    va_log(version_line[!noshadow+!nooutage+!norebuild+(!norebuild && !syncrebuild)]);
    va_log(mvline); /* 3.1 T1: the second line of every create */
    trace_switch();
    VaEngine*e=HeapAlloc(GetProcessHeap(),8,sizeof *e);if(!e)return VA_E_OUTOFMEMORY;
    e->vt=engine_vt;e->refs=1;e->master_rate=e->sh.master_rate=48000;
    /* 2.1 T1: the old positive files VinceAudio.shadow/.outage/.rebuild are ignored, one 'switch:' line per process each. */
    static const char*const legacy[3]={"VinceAudio.shadow","VinceAudio.outage","VinceAudio.rebuild"};static VaU32 legacy_seen;
    for(VaU32 i=0;i<3;++i)if(module_file(path,legacy[i]) && GetFileAttributesW(path)!=0xFFFFFFFFUL
                             && !(__atomic_fetch_or(&legacy_seen,1u<<i,__ATOMIC_ACQ_REL)&(1u<<i))){
        char line[64];if(va_format(line,sizeof line,"switch: %s ignored (legacy)\r\n",legacy[i]))va_log(line);
    }
    int rebuild=!norebuild,outage=!nooutage;
    if(!noshadow){
        cs_ready();EnterCriticalSection(&g_cs);
        e->shadow=1;e->sh.outage=(VaU32)outage;e->sh.rebuild=(VaU32)rebuild;e->sh.syncrebuild=(VaU32)syncrebuild;e->next_shadow=g_shadow_chain;g_shadow_chain=e;
        __atomic_add_fetch(&g_shadow_engines,1,__ATOMIC_ACQ_REL);
        LeaveCriticalSection(&g_cs);
        va_log("shadow: registry on\r\n");
        if(outage){__atomic_add_fetch(&va_outage_refs,1,__ATOMIC_ACQ_REL);va_log("outage: switch on\r\n");
            VaU16 st[8];GetLocalTime(st);char line[64];unsigned k=0; /* 1.6 T1: wall clock beside the t= stamps */
            static const char pre[]="outage: clock local ";for(unsigned i=0;pre[i];++i)line[k++]=pre[i];
            static const VaU8 w[7]={4,2,2,2,2,2,3},f[7]={0,1,3,4,5,6,7};static const char sep[7]={'-','-',' ',':',':','.',0};
            for(int i=0;i<7;++i){VaU32 v=st[f[i]];for(int d=w[i]-1;d>=0;--d){line[k+d]=(char)('0'+v%10);v/=10;}k+=w[i];if(sep[i])line[k++]=sep[i];}
            line[k++]='\r';line[k++]='\n';line[k]=0;va_log(line);
        }
        if(rebuild){
            va_log("rebuild: switch on\r\n");nt_start(); /* 1.5 T4 notifier; outside g_cs */
            nt_live(&e->sh); /* 1.6 T2; with 1.6 T1's g_nt_req (endpoint lines from the notifier thread) */
            e->sh.nt_seen=__atomic_load_n(&g_nt_gen,__ATOMIC_ACQUIRE);e->sh.nt_appear_seen=(VaU32)__atomic_load_n(&g_nt_appear,__ATOMIC_ACQUIRE);
        }
    }
    *out=e;return 0;
}
