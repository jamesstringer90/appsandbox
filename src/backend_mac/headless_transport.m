#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>
#import "asb_ivshmem_transport.h"
#include "headless_vm.h"
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <string.h>

@interface AsbChannelContext:NSObject
@property(nonatomic,strong) AsbIvshmemTransport *memory;
@property(nonatomic,strong) VZVirtioSocketDevice *device;
@property(nonatomic,strong) VZGraphicsDisplay *display;
@end
@implementation AsbChannelContext
@end

void *asb_mac_channel_create(void *memory,void *device,void *display)
{
    AsbChannelContext *context=[AsbChannelContext new];
    context.memory=(__bridge AsbIvshmemTransport *)memory;
    context.device=(__bridge VZVirtioSocketDevice *)device;
    context.display=(__bridge VZGraphicsDisplay *)display;
    return (__bridge_retained void *)context;
}

void *asb_mac_channel_display(void *context)
{
    return (__bridge void *)((__bridge AsbChannelContext *)context).display;
}

void asb_mac_channel_retain(void *context) { if (context) CFRetain(context); }
void asb_mac_channel_release(void *context) { if (context) CFRelease(context); }

int asb_mac_channel_connect(const AsbMacHeadlessTarget *target,unsigned channel,unsigned timeout)
{
    @autoreleasepool {
        AsbChannelContext *context=(__bridge AsbChannelContext *)target->channel;
        if (!context || [NSThread isMainThread]) return -1;
        int fd=-1;
        if (context.memory) fd=[context.memory connectChannel:(int)channel timeoutMs:(int)timeout];
        else if (context.device) {
            NSObject *lock=[NSObject new];__block int result=-1;__block BOOL completed=NO;
            dispatch_semaphore_t ready=dispatch_semaphore_create(0);
            dispatch_async(dispatch_get_main_queue(),^{
                @synchronized(lock) { if (completed) return; }
                [context.device connectToPort:channel completionHandler:^(VZVirtioSocketConnection *connection,NSError *error) {
                    int connected=connection && !error?dup(connection.fileDescriptor):-1;
                    @synchronized(lock) {
                        if (completed) { if (connected>=0) close(connected); }
                        else result=connected;
                    }
                    dispatch_semaphore_signal(ready);
                }];
            });
            long expired=dispatch_semaphore_wait(ready,dispatch_time(DISPATCH_TIME_NOW,(int64_t)timeout*NSEC_PER_MSEC));
            @synchronized(lock) {
                completed=YES;
                if (expired && result>=0) close(result);
                fd=expired?-1:result;
            }
        }
        if (fd>=0) {
            fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
            int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
        }
        return fd;
    }
}

static uint32_t transfer(int fd,void *data,size_t size,int sending)
{
    unsigned char *bytes=data;uint64_t deadline=asb_mac_milliseconds()+12000;
    while (size) {
        uint64_t now=asb_mac_milliseconds();if (now>=deadline) return ASB_TIMEOUT;
        struct pollfd pending={fd,sending?POLLOUT:POLLIN,0};
        int ready=poll(&pending,1,(int)(deadline-now));
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0) return ready?ASB_PIPE:ASB_TIMEOUT;
        ssize_t count=sending?send(fd,bytes,size,0):recv(fd,bytes,size,0);
        if (count<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (count<=0) return ASB_PIPE;
        bytes+=count;size-=(size_t)count;
    }
    return 0;
}

uint32_t asb_mac_channel_read(int fd,void *data,size_t size) { return transfer(fd,data,size,0); }
uint32_t asb_mac_channel_write(int fd,const void *data,size_t size) { return transfer(fd,(void *)data,size,1); }

int asb_mac_input_versions(int fd,int (*cancelled)(void *),void *context,int *keyboardVersion,int *mouseVersion) {
    InputPacket queries[] = {
        { INPUT_MAGIC, INPUT_KEYBOARD_QUERY, INPUT_KEYBOARD_VERSION, arc4random() | 1u, 0 },
        { INPUT_MAGIC, INPUT_MOUSE_QUERY, INPUT_MOUSE_NORMALIZED_VERSION, arc4random() | 1u, 0 }
    };
    uint8_t received[sizeof(uint32_t) + sizeof(InputPacket)];
    size_t sent = 0, count = 0;
    BOOL greeted = NO, keyboardReplied = NO, mouseReplied = NO;
    *keyboardVersion = 1;
    *mouseVersion = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!cancelled(context)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t left = 500000 - ((now.tv_sec - start.tv_sec) * 1000000
                            + (now.tv_nsec - start.tv_nsec) / 1000);
        if (left <= 0) break;
        fd_set reads, writes;
        FD_ZERO(&reads); FD_SET(fd, &reads);
        FD_ZERO(&writes); if (sent < sizeof(queries)) FD_SET(fd, &writes);
        struct timeval timeout = { 0, (suseconds_t)(left < 50000 ? left : 50000) };
        int ready = select(fd + 1, &reads, &writes, NULL, &timeout);
        if (ready < 0) { if (errno == EINTR) continue; return 0; }
        if (FD_ISSET(fd, &writes)) {
            ssize_t n = send(fd, (uint8_t *)queries + sent, sizeof(queries) - sent, MSG_DONTWAIT);
            if (n > 0) sent += (size_t)n;
            else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return 0;
        }
        if (FD_ISSET(fd, &reads)) {
            ssize_t n = recv(fd, received + count, sizeof(received) - count, 0);
            if (n == 0) return 0;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return 0;
            }
            count += (size_t)n;
            if (!greeted && count >= sizeof(uint32_t)) {
                uint32_t magic;
                memcpy(&magic, received, sizeof(magic));
                if (magic != INPUT_READY_MAGIC) return sent == sizeof(queries);
                greeted = YES;
                count -= sizeof(magic);
                memmove(received, received + sizeof(magic), count);
            }
            if (greeted && count >= sizeof(InputPacket)) {
                InputPacket reply;
                memcpy(&reply, received, sizeof(reply));
                count -= sizeof(reply);
                memmove(received, received + sizeof(reply), count);
                if (reply.magic == INPUT_MAGIC && reply.param3 == 0) {
                    if (reply.type == INPUT_KEYBOARD_REPLY && reply.param2 == queries[0].param2) {
                        *keyboardVersion = reply.param1 == INPUT_KEYBOARD_VERSION ? INPUT_KEYBOARD_VERSION : 1;
                        keyboardReplied = YES;
                    } else if (reply.type == INPUT_MOUSE_REPLY && reply.param2 == queries[1].param2) {
                        *mouseVersion = reply.param1 >= INPUT_MOUSE_VERSION &&
                            reply.param1 <= INPUT_MOUSE_NORMALIZED_VERSION ? (int)reply.param1 : 0;
                        mouseReplied = YES;
                    }
                }
            }
        }
        if (sent == sizeof(queries) && keyboardReplied && mouseReplied)
            return *mouseVersion >= INPUT_MOUSE_VERSION && count ? 0 : 1;
    }
    if (*mouseVersion >= INPUT_MOUSE_VERSION && count) return 0;
    return sent && sent < sizeof(queries) ? 0 : 1;
}
