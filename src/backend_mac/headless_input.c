#include "headless_vm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct AsbMacInput {
    atomic_int references,stop;
    pthread_mutex_t lock,actions;
    pthread_cond_t changed;
    pthread_t thread;
    AsbMacHeadlessTarget target;
    AsbInputRouter router;
    AsbMacInputStatus labels;
    int fd,viewer,claiming,retired,connected;
    uint32_t keyboard,mouse,barrier_done;
};

static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static AsbMacInput *sessions[32];

uint64_t asb_mac_milliseconds(void)
{
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static void wait_changed(AsbMacInput *s,unsigned ms)
{
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_sec+=ms/1000;until.tv_nsec+=(long)(ms%1000)*1000000;
    if (until.tv_nsec>=1000000000) { ++until.tv_sec;until.tv_nsec-=1000000000; }
    pthread_cond_timedwait(&s->changed,&s->lock,&until);
}

static int emit(void *context,const InputPacket *packet)
{
    AsbMacInput *s=context;
    if (s->fd<0 || !s->connected) return 0;
    ssize_t n=send(s->fd,packet,sizeof(*packet),0);
    if (n==(ssize_t)sizeof(*packet)) return 1;
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    shutdown(s->fd,SHUT_RDWR);return 0;
}

void asb_mac_input_release(AsbMacInput *s)
{
    if (s && atomic_fetch_sub(&s->references,1)==1) {
        asb_mac_channel_release(s->target.channel);
        pthread_mutex_destroy(&s->lock);pthread_mutex_destroy(&s->actions);
        pthread_cond_destroy(&s->changed);free(s);
    }
}

static int input_stopped(void *context)
{
    return atomic_load(&((AsbMacInput *)context)->stop)!=0;
}

static void *input_worker(void *context)
{
    AsbMacInput *s=context;
    while (!atomic_load(&s->stop)) {
        pthread_mutex_lock(&s->lock);
        int needed=s->viewer>=0 || s->claiming || s->router.working || s->router.transition;
        if (!needed) wait_changed(s,100);
        pthread_mutex_unlock(&s->lock);
        if (!needed) continue;
        int fd=asb_mac_channel_connect(&s->target,3,1000);
        if (fd<0) { usleep(100000);continue; }
        int keyboard=1,mouse=0;
        pthread_mutex_lock(&s->lock);
        if (s->retired) { pthread_mutex_unlock(&s->lock);close(fd);break; }
        s->fd=fd;pthread_mutex_unlock(&s->lock);
        int good=asb_mac_input_versions(fd,input_stopped,s,&keyboard,&mouse);
        if (!good) {
            pthread_mutex_lock(&s->lock);s->fd=-1;pthread_mutex_unlock(&s->lock);
            close(fd);usleep(100000);continue;
        }
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
        pthread_mutex_lock(&s->lock);
        s->fd=fd;s->connected=1;s->keyboard=keyboard;s->mouse=mouse;
        pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
        InputPacket response;size_t count=0;
        while (!atomic_load(&s->stop)) {
            pthread_mutex_lock(&s->lock);
            needed=s->viewer>=0 || s->claiming || s->router.working || s->router.transition;
            pthread_mutex_unlock(&s->lock);
            if (!needed) break;
            struct pollfd pending={fd,POLLIN,0};
            int result=poll(&pending,1,10);
            if (result<0 && errno==EINTR) continue;
            if (result<0) break;
            if (!result) continue;
            ssize_t n=recv(fd,(char *)&response+count,sizeof(response)-count,0);
            if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
            if (n<=0) break;
            count+=(size_t)n;
            if (count<sizeof(response)) continue;
            count=0;
            if (response.magic!=INPUT_MAGIC) break;
            pthread_mutex_lock(&s->lock);
            asb_input_router_reply(&s->router,&response);
            if (response.type==INPUT_MOUSE_REPLY && (response.param2&0x80000000u)) s->barrier_done=response.param2;
            else if (s->viewer>=0 && send(s->viewer,&response,sizeof(response),0)!=(ssize_t)sizeof(response))
                shutdown(s->viewer,SHUT_RDWR);
            pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
        }
        pthread_mutex_lock(&s->lock);
        s->fd=-1;s->connected=0;s->keyboard=1;s->mouse=0;
        asb_input_router_disconnect(&s->router);
        if (s->viewer>=0) shutdown(s->viewer,SHUT_RDWR);
        pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
        close(fd);
    }
    return NULL;
}

static void retire(AsbMacInput *s)
{
    pthread_mutex_lock(&s->lock);s->retired=1;atomic_store(&s->stop,1);
    if (s->fd>=0) shutdown(s->fd,SHUT_RDWR);
    if (s->viewer>=0) shutdown(s->viewer,SHUT_RDWR);
    asb_input_router_disconnect(&s->router);s->router.working=s->router.paused=0;
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread,NULL);asb_mac_input_release(s);
}

AsbMacInput *asb_mac_input_session(const AsbMacHeadlessTarget *target)
{
    AsbMacInput *result=NULL,*old=NULL;
    pthread_mutex_lock(&registry_lock);
    int empty=-1;
    for (int i=0;i<32;++i) {
        if (!sessions[i]) { if (empty<0) empty=i;continue; }
        if (sessions[i]->target.id!=target->id) continue;
        if (sessions[i]->target.run_id==target->run_id) result=sessions[i];
        else { old=sessions[i];sessions[i]=NULL;empty=i; }
        break;
    }
    if (!result && empty>=0) {
        result=calloc(1,sizeof(*result));
        if (result) {
            atomic_init(&result->references,1);result->fd=result->viewer=-1;
            pthread_mutex_init(&result->lock,NULL);pthread_mutex_init(&result->actions,NULL);pthread_cond_init(&result->changed,NULL);
            result->target=*target;asb_mac_channel_retain(target->channel);
            asb_input_router_init(&result->router,emit,result);
            if (pthread_create(&result->thread,NULL,input_worker,result)) { asb_mac_input_release(result);result=NULL; }
            else sessions[empty]=result;
        }
    }
    if (result) atomic_fetch_add(&result->references,1);
    pthread_mutex_unlock(&registry_lock);
    if (old) retire(old);
    return result;
}

void asb_mac_input_status(AsbMacInput *s,AsbMacInputStatus *out)
{
    memset(out,0,sizeof(*out));out->flags=ASB_STATUS_AGENT_FINISHED|ASB_STATUS_USER_HAS_CONTROL;
    out->x=out->y=UINT32_MAX;out->keyboard_version=1;
    if (!s) return;
    pthread_mutex_lock(&s->lock);*out=s->labels;
    out->flags=asb_input_router_flags(&s->router);out->generation=s->router.generation;out->work_id=s->router.work_id;
    out->x=s->router.cursor_x;out->y=s->router.cursor_y;out->keyboard_version=s->keyboard;out->mouse_version=s->mouse;
    out->connected=s->connected;pthread_mutex_unlock(&s->lock);
}

void asb_mac_input_context(uint64_t id,AsbMacInputStatus *out)
{
    pthread_mutex_lock(&registry_lock);AsbMacInput *s=NULL;
    for (int i=0;i<32;++i) if (sessions[i] && sessions[i]->target.id==id) { s=sessions[i];break; }
    asb_mac_input_status(s,out);pthread_mutex_unlock(&registry_lock);
}

uint32_t asb_mac_input_claim(AsbMacInput *s,const AsbMacInputStatus *labels)
{
    if (!s) return ASB_MEMORY;
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->retired?ASB_NOT_READY:s->router.cancelled?ASB_CANCELLED:0;
    uint64_t deadline=asb_mac_milliseconds()+10000;++s->claiming;
    while (!error && !s->connected && !s->retired && asb_mac_milliseconds()<deadline) wait_changed(s,100);
    if (!error && (s->retired || !s->connected)) error=ASB_NOT_READY;
    int fresh=!s->router.working;
    if (!error) {
        uint32_t work=s->router.work_id;
        if (fresh) {
            work=arc4random_uniform(0x7ffffffe)+1;
            uint32_t width=s->labels.width,height=s->labels.height;s->labels=*labels;
            s->labels.width=width;s->labels.height=height;
        }
        error=asb_input_router_request(&s->router,work);
    }
    while (!error && !s->retired && s->router.transition && asb_mac_milliseconds()<deadline) wait_changed(s,100);
    if (!error && s->retired) error=ASB_NOT_READY;
    if (!error && s->router.transition) {
        error=ASB_TIMEOUT;if (s->fd>=0) shutdown(s->fd,SHUT_RDWR);
        asb_input_router_disconnect(&s->router);if (fresh) s->router.working=s->router.paused=0;
    }
    --s->claiming;pthread_mutex_unlock(&s->lock);return error;
}

uint32_t asb_mac_input_control(AsbMacInput *s,uint32_t command,uint32_t work)
{
    if (!s) return ASB_NOT_READY;
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->retired?ASB_NOT_READY:asb_input_router_control(&s->router,command,work);
    if (error==ASB_WRITE && s->fd<0 && command!=ASB_INPUT_USER_GRANT) error=0;
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);return error;
}

uint32_t asb_mac_input_send(AsbMacInput *s,int agent,uint32_t lease,const InputPacket *packet)
{
    if (!s) return ASB_NOT_READY;
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->retired?ASB_NOT_READY:asb_input_router_send(&s->router,agent,lease,packet);
    pthread_mutex_unlock(&s->lock);return error;
}

uint32_t asb_mac_input_barrier(AsbMacInput *s,uint32_t lease,unsigned timeout)
{
    if (!s) return ASB_NOT_READY;
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->retired?ASB_NOT_READY:s->router.cancelled?ASB_CANCELLED:
        s->router.generation!=lease || s->router.owner!=ASB_CONTROL_AGENT?ASB_DENIED:0;
    uint32_t id=0;
    if (!error) {
        id=(++s->router.sequence)|0x80000000u;
        InputPacket packet={INPUT_MAGIC,INPUT_MOUSE_QUERY,INPUT_MOUSE_NORMALIZED_VERSION,id,0};
        if (!emit(s,&packet)) error=ASB_WRITE;
    }
    uint64_t deadline=asb_mac_milliseconds()+timeout;
    while (!error && s->barrier_done!=id && s->router.generation==lease && asb_mac_milliseconds()<deadline) wait_changed(s,50);
    if (!error) error=s->router.cancelled?ASB_CANCELLED:s->router.generation!=lease?ASB_DENIED:s->barrier_done==id?0:ASB_TIMEOUT;
    pthread_mutex_unlock(&s->lock);return error;
}

uint32_t asb_mac_input_begin_batch(AsbMacInput *s) { return s && !pthread_mutex_trylock(&s->actions)?0:ASB_BUSY; }
void asb_mac_input_end_batch(AsbMacInput *s) { pthread_mutex_unlock(&s->actions); }

static void *viewer_worker(void *context)
{
    AsbMacInput *s=context;int fd;
    pthread_mutex_lock(&s->lock);fd=s->viewer;uint64_t deadline=asb_mac_milliseconds()+2000;
    while (!s->connected && !s->retired && asb_mac_milliseconds()<deadline) wait_changed(s,50);
    int ready=s->connected;pthread_mutex_unlock(&s->lock);
    uint32_t magic=INPUT_READY_MAGIC;
    if (ready && !asb_mac_channel_write(fd,&magic,sizeof(magic))) {
        InputPacket packet;
        while (!atomic_load(&s->stop)) {
            struct pollfd pending={fd,POLLIN,0};
            int ready=poll(&pending,1,1000);
            if (ready<0 && errno==EINTR) continue;
            if (ready<0) break;
            if (!ready) continue;
            if (asb_mac_channel_read(fd,&packet,sizeof(packet)) || packet.magic!=INPUT_MAGIC) break;
            pthread_mutex_lock(&s->lock);
            if (packet.type==INPUT_KEYBOARD_QUERY || packet.type==INPUT_MOUSE_QUERY) {
                InputPacket response={INPUT_MAGIC,packet.type==INPUT_KEYBOARD_QUERY?INPUT_KEYBOARD_REPLY:INPUT_MOUSE_REPLY,
                    packet.type==INPUT_KEYBOARD_QUERY?s->keyboard:s->mouse,packet.param2,0};
                if (send(fd,&response,sizeof(response),0)!=(ssize_t)sizeof(response)) { pthread_mutex_unlock(&s->lock);break; }
            } else asb_input_router_send(&s->router,0,0,&packet);
            pthread_mutex_unlock(&s->lock);
        }
    }
    pthread_mutex_lock(&s->lock);s->viewer=-1;pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
    close(fd);asb_mac_input_release(s);return NULL;
}

int asb_mac_input_viewer(AsbMacInput *s)
{
    if (!s) return -1;
    int pair[2];if (socketpair(AF_UNIX,SOCK_STREAM,0,pair)) return -1;
    int yes=1;
    setsockopt(pair[0],SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));setsockopt(pair[1],SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
    fcntl(pair[0],F_SETFD,FD_CLOEXEC);fcntl(pair[1],F_SETFD,FD_CLOEXEC);
    fcntl(pair[0],F_SETFL,fcntl(pair[0],F_GETFL)|O_NONBLOCK);
    pthread_mutex_lock(&s->lock);
    if (s->viewer>=0 || s->retired) { pthread_mutex_unlock(&s->lock);close(pair[0]);close(pair[1]);return -1; }
    s->viewer=pair[0];atomic_fetch_add(&s->references,1);pthread_t thread;
    if (pthread_create(&thread,NULL,viewer_worker,s)) {
        s->viewer=-1;pthread_mutex_unlock(&s->lock);close(pair[0]);close(pair[1]);asb_mac_input_release(s);return -1;
    }
    pthread_detach(thread);pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);return pair[1];
}

void asb_mac_input_dimensions(uint64_t id,unsigned width,unsigned height)
{
    pthread_mutex_lock(&registry_lock);
    for (int i=0;i<32;++i) if (sessions[i] && sessions[i]->target.id==id) {
        pthread_mutex_lock(&sessions[i]->lock);sessions[i]->labels.width=width;sessions[i]->labels.height=height;
        pthread_mutex_unlock(&sessions[i]->lock);break;
    }
    pthread_mutex_unlock(&registry_lock);
}

void asb_mac_input_forget(uint64_t id)
{
    AsbMacInput *removed[32];unsigned count=0;
    pthread_mutex_lock(&registry_lock);
    for (int i=0;i<32;++i) if (sessions[i] && (!id || sessions[i]->target.id==id)) { removed[count++]=sessions[i];sessions[i]=NULL; }
    pthread_mutex_unlock(&registry_lock);
    for (unsigned i=0;i<count;++i) retire(removed[i]);
    asb_mac_ssh_shutdown(id);
}
