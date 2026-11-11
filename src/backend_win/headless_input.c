#define _CRT_RAND_S
#include "headless_vm.h"
#include "headless_ssh.h"
#include <stdlib.h>
#include <string.h>

struct AsbInputSession {
    volatile LONG refs;
    SRWLOCK lock;
    SRWLOCK actions;
    CONDITION_VARIABLE changed;
    AsbHeadlessTarget target;
    AsbInputRouter router;
    AsbInputStatus labels;
    SOCKET connection,negotiating;
    HANDLE thread,stop;
    UINT32 keyboard,mouse,barrier_done;
    InputPacket replies[64];
    unsigned head,tail;
    unsigned viewers,claiming,retired;
};

static SRWLOCK registry_lock=SRWLOCK_INIT;
static AsbInputSession *sessions[ASB_MAX_VMS];

static void retire(AsbInputSession *s)
{
    AcquireSRWLockExclusive(&s->lock);
    s->retired=1;
    SetEvent(s->stop);
    if (s->connection!=INVALID_SOCKET) shutdown(s->connection,SD_BOTH);
    if (s->negotiating!=INVALID_SOCKET) shutdown(s->negotiating,SD_BOTH);
    asb_input_router_disconnect(&s->router);
    s->router.working=s->router.paused=0;
    WakeAllConditionVariable(&s->changed);
    ReleaseSRWLockExclusive(&s->lock);
    WaitForSingleObject(s->thread,INFINITE);
    CloseHandle(s->thread);CloseHandle(s->stop);
    asb_input_release(s);
}

static int emit(void *context,const InputPacket *packet)
{
    AsbInputSession *s=context;
    if (s->connection==INVALID_SOCKET) return 0;
    int n=send(s->connection,(const char *)packet,sizeof(*packet),0);
    if (n==sizeof(*packet)) return 1;
    if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return 0;
    shutdown(s->connection,SD_BOTH);
    return 0;
}

static BOOL negotiate(AsbInputSession *session,SOCKET fd,UINT32 *keyboard,UINT32 *mouse,InputPacket *reply,int *got)
{
    static volatile LONG request_id;
    UINT32 ready=0;
    if (asb_channel_read(fd,&ready,sizeof(ready)) || ready!=INPUT_READY_MAGIC) return FALSE;
    u_long nonblock=1;DWORD timeout=0;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(char *)&timeout,sizeof(timeout));
    if (ioctlsocket(fd,FIONBIO,&nonblock)) return FALSE;
    UINT32 request=(UINT32)InterlockedIncrement(&request_id);
    InputPacket queries[]={{INPUT_MAGIC,INPUT_KEYBOARD_QUERY,INPUT_KEYBOARD_VERSION,request,0},
        {INPUT_MAGIC,INPUT_MOUSE_QUERY,INPUT_MOUSE_NORMALIZED_VERSION,request,0}};
    if (send(fd,(const char *)queries,sizeof(queries),0)!=sizeof(queries)) return FALSE;
    BOOL keyboard_reply=FALSE,mouse_reply=FALSE;
    ULONGLONG deadline=GetTickCount64()+500;
    while ((!keyboard_reply || !mouse_reply) && WaitForSingleObject(session->stop,0)!=WAIT_OBJECT_0) {
        LONGLONG remaining=(LONGLONG)(deadline-GetTickCount64());
        if (remaining<=0) break;
        fd_set reads;FD_ZERO(&reads);FD_SET(fd,&reads);
        struct timeval wait={0,(long)min(remaining,50)*1000};
        int result=select(0,&reads,NULL,NULL,&wait);
        if (result==SOCKET_ERROR) return FALSE;
        if (!result) continue;
        int n=recv(fd,(char *)reply+*got,(int)sizeof(*reply)-*got,0);
        if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) continue;
        if (n<=0) return FALSE;
        *got+=n;
        if (*got!=(int)sizeof(*reply)) continue;
        if (reply->magic==INPUT_MAGIC && reply->param2==request && !reply->param3) {
            if (reply->type==INPUT_KEYBOARD_REPLY) {
                keyboard_reply=TRUE;
                if (reply->param1==INPUT_KEYBOARD_VERSION) *keyboard=INPUT_KEYBOARD_VERSION;
            } else if (reply->type==INPUT_MOUSE_REPLY) {
                mouse_reply=TRUE;
                if (reply->param1>=INPUT_MOUSE_VERSION && reply->param1<=INPUT_MOUSE_NORMALIZED_VERSION) *mouse=reply->param1;
            }
        }
        *got=0;
    }
    return WaitForSingleObject(session->stop,0)!=WAIT_OBJECT_0;
}

static DWORD WINAPI worker(void *context)
{
    AsbInputSession *s=context;
    while (WaitForSingleObject(s->stop,0)!=WAIT_OBJECT_0) {
        AcquireSRWLockShared(&s->lock);
        BOOL needed=s->viewers || s->claiming || s->router.working || s->router.transition;
        ReleaseSRWLockShared(&s->lock);
        if (!needed) { WaitForSingleObject(s->stop,100);continue; }
        SOCKET fd=asb_channel_connect(&s->target,3,1000);
        InputPacket packet={0};int count=0;
        UINT32 keyboard=1,mouse=0;
        if (fd==INVALID_SOCKET) goto retry;
        AcquireSRWLockExclusive(&s->lock);
        if (s->retired) { ReleaseSRWLockExclusive(&s->lock);closesocket(fd);break; }
        s->negotiating=fd;ReleaseSRWLockExclusive(&s->lock);
        BOOL connected=negotiate(s,fd,&keyboard,&mouse,&packet,&count);
        AcquireSRWLockExclusive(&s->lock);s->negotiating=INVALID_SOCKET;ReleaseSRWLockExclusive(&s->lock);
        if (!connected) goto close;
        AcquireSRWLockExclusive(&s->lock);
        s->connection=fd;s->keyboard=keyboard;s->mouse=mouse;
        WakeAllConditionVariable(&s->changed);
        ReleaseSRWLockExclusive(&s->lock);
        while (WaitForSingleObject(s->stop,0)!=WAIT_OBJECT_0) {
            AcquireSRWLockShared(&s->lock);
            needed=s->viewers || s->claiming || s->router.working || s->router.transition;
            ReleaseSRWLockShared(&s->lock);
            if (!needed) break;
            int n=recv(fd,(char *)&packet+count,sizeof(packet)-count,0);
            if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) { WaitForSingleObject(s->stop,5);continue; }
            if (n<=0) break;
            count+=n;
            if (count!=sizeof(packet)) continue;
            count=0;
            if (packet.magic!=INPUT_MAGIC) break;
            AcquireSRWLockExclusive(&s->lock);
            asb_input_router_reply(&s->router,&packet);
            if (packet.type==INPUT_MOUSE_REPLY && (packet.param2&0x80000000u)) {
                s->barrier_done=packet.param2;
            } else if (s->tail-s->head<ARRAYSIZE(s->replies)) {
                s->replies[s->tail++%ARRAYSIZE(s->replies)]=packet;
            }
            WakeAllConditionVariable(&s->changed);
            ReleaseSRWLockExclusive(&s->lock);
        }
        AcquireSRWLockExclusive(&s->lock);
        s->connection=INVALID_SOCKET;s->keyboard=1;s->mouse=0;
        asb_input_router_disconnect(&s->router);
        WakeAllConditionVariable(&s->changed);
        ReleaseSRWLockExclusive(&s->lock);
close:
        closesocket(fd);
retry:
        if (WaitForSingleObject(s->stop,1000)==WAIT_OBJECT_0) break;
    }
    return 0;
}

ASB_API AsbInputSession *asb_input_session(const AsbHeadlessTarget *target)
{
    AsbInputSession *result=NULL;
    AsbInputSession *previous=NULL;
    AcquireSRWLockExclusive(&registry_lock);
    int free_slot=-1;
    for (int i=0;i<ASB_MAX_VMS;++i) {
        if (!sessions[i]) { if (free_slot<0) free_slot=i;continue; }
        if (sessions[i]->target.id==target->id) {
            if (IsEqualGUID(&sessions[i]->target.runtime,&target->runtime)) result=sessions[i];
            else { previous=sessions[i];sessions[i]=NULL;free_slot=i; }
            break;
        }
    }
    if (!result && free_slot>=0) {
        result=calloc(1,sizeof(*result));
        if (result) {
            result->target=*target;
            result->refs=1;
            result->connection=result->negotiating=INVALID_SOCKET;
            InitializeSRWLock(&result->lock);
            InitializeSRWLock(&result->actions);
            InitializeConditionVariable(&result->changed);
            asb_input_router_init(&result->router,emit,result);
            result->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
            result->thread=result->stop?CreateThread(NULL,0,worker,result,0,NULL):NULL;
            if (!result->thread) {
                if (result->stop) CloseHandle(result->stop);
                free(result);result=NULL;
            } else sessions[free_slot]=result;
        }
    }
    if (result) InterlockedIncrement(&result->refs);
    ReleaseSRWLockExclusive(&registry_lock);
    if (previous) retire(previous);
    return result;
}

ASB_API void asb_input_release(AsbInputSession *s)
{
    if (s && !InterlockedDecrement(&s->refs)) free(s);
}

ASB_API void asb_input_status(AsbInputSession *s,AsbInputStatus *out)
{
    memset(out,0,sizeof(*out));
    out->connection=INVALID_SOCKET;
    out->flags=ASB_STATUS_AGENT_FINISHED|ASB_STATUS_USER_HAS_CONTROL;
    out->x=out->y=UINT32_MAX;out->keyboard_version=1;
    if (!s) return;
    AcquireSRWLockShared(&s->lock);
    *out=s->labels;
    out->flags=asb_input_router_flags(&s->router);
    out->generation=s->router.generation;out->work_id=s->router.work_id;
    out->x=s->router.cursor_x;out->y=s->router.cursor_y;
    out->connection=s->connection;out->keyboard_version=s->keyboard;out->mouse_version=s->mouse;
    ReleaseSRWLockShared(&s->lock);
}

ASB_API void asb_input_viewer(AsbInputSession *s,BOOL attached)
{
    if (!s) return;
    AcquireSRWLockExclusive(&s->lock);
    if (attached) ++s->viewers;
    else if (s->viewers) --s->viewers;
    ReleaseSRWLockExclusive(&s->lock);
}

ASB_API DWORD asb_input_claim(AsbInputSession *s,const AsbInputStatus *labels)
{
    if (!s) return ERROR_NOT_ENOUGH_MEMORY;
    AcquireSRWLockExclusive(&s->lock);
    if (s->retired) { ReleaseSRWLockExclusive(&s->lock);return ERROR_NOT_READY; }
    if (s->router.cancelled) { ReleaseSRWLockExclusive(&s->lock);return ERROR_CANCELLED; }
    ULONGLONG deadline=GetTickCount64()+10000;
    ++s->claiming;
    BOOL new_work=!s->router.working;
    while (!s->retired && s->connection==INVALID_SOCKET && GetTickCount64()<deadline)
        SleepConditionVariableSRW(&s->changed,&s->lock,100,0);
    DWORD error=s->retired || s->connection==INVALID_SOCKET?ERROR_NOT_READY:0;
    if (!error) {
        UINT32 work=s->router.work_id;
        if (!s->router.working) {
            unsigned random=0;
            if (rand_s(&random)) random=(unsigned)GetTickCount64();
            work=random%0x7ffffffe+1;
            s->labels=*labels;
        }
        error=asb_input_router_request(&s->router,work);
    }
    while (!error && !s->retired && s->router.transition && GetTickCount64()<deadline)
        SleepConditionVariableSRW(&s->changed,&s->lock,100,0);
    if (!error && s->retired) error=ERROR_NOT_READY;
    if (!error && s->router.transition) {
        error=ERROR_TIMEOUT;
        if (s->connection!=INVALID_SOCKET) shutdown(s->connection,SD_BOTH);
        asb_input_router_disconnect(&s->router);
        if (new_work) s->router.working=s->router.paused=0;
        WakeAllConditionVariable(&s->changed);
    }
    --s->claiming;
    ReleaseSRWLockExclusive(&s->lock);
    return error;
}

ASB_API DWORD asb_input_control(AsbInputSession *s,UINT32 command,UINT32 work)
{
    if (!s) return ERROR_INVALID_HANDLE;
    AcquireSRWLockExclusive(&s->lock);
    DWORD error=s->retired?ERROR_NOT_READY:asb_input_router_control(&s->router,command,work);
    if (error==ERROR_WRITE_FAULT && s->connection==INVALID_SOCKET && command!=ASB_INPUT_USER_GRANT) error=0;
    WakeAllConditionVariable(&s->changed);
    ReleaseSRWLockExclusive(&s->lock);
    return error;
}

ASB_API DWORD asb_input_send(AsbInputSession *s,BOOL agent,UINT32 lease,const InputPacket *p)
{
    if (!s) return ERROR_INVALID_HANDLE;
    AcquireSRWLockExclusive(&s->lock);
    DWORD error=s->retired?ERROR_NOT_READY:asb_input_router_send(&s->router,agent,lease,p);
    ReleaseSRWLockExclusive(&s->lock);
    return error;
}

ASB_API BOOL asb_input_poll(AsbInputSession *s,InputPacket *p)
{
    if (!s) return FALSE;
    AcquireSRWLockExclusive(&s->lock);
    BOOL result=s->head!=s->tail;
    if (result) *p=s->replies[s->head++%ARRAYSIZE(s->replies)];
    ReleaseSRWLockExclusive(&s->lock);
    return result;
}

ASB_API DWORD asb_input_barrier(AsbInputSession *s,UINT32 lease,DWORD timeout)
{
    if (!s) return ERROR_INVALID_HANDLE;
    AcquireSRWLockExclusive(&s->lock);
    DWORD error=s->retired?ERROR_NOT_READY:s->router.cancelled?ERROR_CANCELLED:
        s->router.generation!=lease || s->router.owner!=ASB_CONTROL_AGENT?ERROR_ACCESS_DENIED:0;
    UINT32 id=0;
    if (!error) {
        id=(++s->router.sequence)|0x80000000u;
        InputPacket packet={INPUT_MAGIC,INPUT_MOUSE_QUERY,INPUT_MOUSE_NORMALIZED_VERSION,id,0};
        if (!emit(s,&packet)) error=ERROR_WRITE_FAULT;
    }
    ULONGLONG deadline=GetTickCount64()+timeout;
    while (!error && s->barrier_done!=id && GetTickCount64()<deadline && s->router.generation==lease)
        SleepConditionVariableSRW(&s->changed,&s->lock,50,0);
    if (!error) error=s->router.cancelled?ERROR_CANCELLED:s->router.generation!=lease?ERROR_ACCESS_DENIED:
        s->barrier_done==id?0:ERROR_TIMEOUT;
    ReleaseSRWLockExclusive(&s->lock);
    return error;
}

ASB_API DWORD asb_input_begin_batch(AsbInputSession *s)
{
    return s && TryAcquireSRWLockExclusive(&s->actions)?0:ERROR_BUSY;
}

ASB_API void asb_input_end_batch(AsbInputSession *s)
{
    ReleaseSRWLockExclusive(&s->actions);
}

ASB_API void asb_input_context(AsbVmId id,AsbInputStatus *context)
{
    memset(context,0,sizeof(*context));
    context->flags=ASB_STATUS_AGENT_FINISHED|ASB_STATUS_USER_HAS_CONTROL;
    context->x=context->y=UINT32_MAX;
    AcquireSRWLockShared(&registry_lock);
    for (int i=0;i<ASB_MAX_VMS;++i)
        if (sessions[i] && sessions[i]->target.id==id) { asb_input_status(sessions[i],context);break; }
    ReleaseSRWLockShared(&registry_lock);
}

ASB_API void asb_headless_forget(AsbVmId id)
{
    AsbInputSession *removed[ASB_MAX_VMS];
    unsigned count=0;
    AcquireSRWLockExclusive(&registry_lock);
    for (int i=0;i<ASB_MAX_VMS;++i) if (sessions[i] && (!id || sessions[i]->target.id==id)) {
        removed[count++]=sessions[i];
        sessions[i]=NULL;
    }
    ReleaseSRWLockExclusive(&registry_lock);
    for (unsigned i=0;i<count;++i) retire(removed[i]);
    asb_ssh_shutdown(id);
}

ASB_API void asb_headless_shutdown(void)
{
    asb_headless_forget(0);
}
