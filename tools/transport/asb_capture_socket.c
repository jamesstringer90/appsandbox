#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "asb_capture_socket.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct AsbCaptureSocket {
    int listener,client,stopped,wake[2];
    pthread_t thread;
    pthread_mutex_t lock;
    AsbCaptureService service;
};

static uint64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static int stopped(AsbCaptureSocket *s)
{
    return __atomic_load_n(&s->stopped,__ATOMIC_ACQUIRE);
}

static int transfer(AsbCaptureSocket *s,int fd,void *buffer,size_t size,int sending)
{
    uint64_t deadline=milliseconds()+2000;
    while (!stopped(s) && milliseconds()<deadline) {
        struct pollfd pending={.fd=fd,.events=sending?POLLOUT:POLLIN};
        int result=poll(&pending,1,100);
        if (result<0 && errno!=EINTR) return -1;
        if (result<=0) continue;
        ssize_t count=sending?send(fd,buffer,size,MSG_NOSIGNAL|MSG_DONTWAIT)
                             :recv(fd,buffer,size,MSG_DONTWAIT);
        if (count>=0) return (int)count;
        if (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) return -1;
    }
    return -1;
}

static int receive_bytes(void *context,void *connection,void *buffer,size_t size)
{
    return transfer(context,*(int *)connection,buffer,size,0);
}

static int send_bytes(void *context,void *connection,const void *buffer,size_t size)
{
    return transfer(context,*(int *)connection,(void *)buffer,size,1);
}

static uint32_t capture(void *context,const AsbScreenshotRequest *request,
                        AsbScreenshotReply *reply,unsigned char **pixels)
{
    AsbCaptureSocket *s=context;
    return s->service.capture(s->service.context,request,reply,pixels);
}

static void *worker(void *argument)
{
    AsbCaptureSocket *s=argument;
    int priority=nice(10);
    (void)priority;
    AsbCaptureService service={s,receive_bytes,send_bytes,capture};
    while (!stopped(s)) {
        struct pollfd pending[]={{.fd=s->listener,.events=POLLIN},{.fd=s->wake[0],.events=POLLIN}};
        int result=poll(pending,2,-1);
        if (result<0 && errno!=EINTR) break;
        if (result<=0 || stopped(s)) continue;
        int client=accept(s->listener,NULL,NULL);
        if (client<0) {
            if (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK) continue;
            break;
        }
        fcntl(client,F_SETFL,fcntl(client,F_GETFL)|O_NONBLOCK);
        fcntl(client,F_SETFD,FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int no_signal=1;
        setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&no_signal,sizeof(no_signal));
#endif
        pthread_mutex_lock(&s->lock);
        s->client=client;
        pthread_mutex_unlock(&s->lock);
        asb_capture_service_request(&service,&client);
        pthread_mutex_lock(&s->lock);
        close(client);
        s->client=-1;
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

AsbCaptureSocket *asb_capture_socket_start(int listener,void *context,
    uint32_t (*capture_frame)(void *,const AsbScreenshotRequest *,AsbScreenshotReply *,unsigned char **))
{
    AsbCaptureSocket *s=calloc(1,sizeof(*s));
    if (!s) { close(listener);return NULL; }
    s->listener=listener;s->client=-1;
    if (pipe(s->wake)<0) { close(listener);free(s);return NULL; }
    fcntl(s->wake[0],F_SETFD,FD_CLOEXEC);
    fcntl(s->wake[1],F_SETFD,FD_CLOEXEC);
    s->service.context=context;s->service.capture=capture_frame;
    pthread_mutex_init(&s->lock,NULL);
    fcntl(listener,F_SETFL,fcntl(listener,F_GETFL)|O_NONBLOCK);
    fcntl(listener,F_SETFD,FD_CLOEXEC);
    if (pthread_create(&s->thread,NULL,worker,s)) {
        pthread_mutex_destroy(&s->lock);
        close(s->wake[0]);close(s->wake[1]);
        close(listener);free(s);return NULL;
    }
    return s;
}

void asb_capture_socket_stop(AsbCaptureSocket *s)
{
    if (!s) return;
    __atomic_store_n(&s->stopped,1,__ATOMIC_RELEASE);
    char notification=1;
    ssize_t notified=write(s->wake[1],&notification,1);
    (void)notified;
    pthread_mutex_lock(&s->lock);
    shutdown(s->listener,SHUT_RDWR);
    if (s->client>=0) shutdown(s->client,SHUT_RDWR);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread,NULL);
    close(s->listener);
    close(s->wake[0]);close(s->wake[1]);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
