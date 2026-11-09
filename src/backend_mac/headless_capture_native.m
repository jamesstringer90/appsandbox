#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Virtualization/Virtualization.h>
#import <objc/runtime.h>
#include "headless_vm.h"
#include "../../tools/transport/asb_capture_service.h"
#include <stdlib.h>

@interface AsbDisplayCapture:NSObject
@property(nonatomic) BOOL pending;
@end
@implementation AsbDisplayCapture
@end

static const char capture_state_key;

uint32_t asb_mac_native_capture(const AsbMacHeadlessTarget *target,unsigned timeout,AsbScreenshotReply *reply,unsigned char **pixels)
{
    *pixels=NULL;
    if (!timeout || timeout>10000 || NSThread.isMainThread) return ASB_INVALID;
    @autoreleasepool {
        VZGraphicsDisplay *display=(__bridge VZGraphicsDisplay *)asb_mac_channel_display(target->channel);
        if (!display) return ASB_NOT_READY;
        NSObject *lock=[NSObject new];dispatch_semaphore_t ready=dispatch_semaphore_create(0);
        __block id screenshot=nil;__block BOOL expired=NO;__block uint32_t error=ASB_NOT_READY;
        dispatch_async(dispatch_get_main_queue(),^{
            @synchronized(lock) { if (expired) return; }
            SEL selector=NSSelectorFromString(@"_takeScreenshotWithCompletionHandler:");
            if (![display respondsToSelector:selector]) {
                @synchronized(lock) { error=120; }
                dispatch_semaphore_signal(ready);return;
            }
            AsbDisplayCapture *state=objc_getAssociatedObject(display,&capture_state_key);
            if (!state) {
                state=[AsbDisplayCapture new];objc_setAssociatedObject(display,&capture_state_key,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            @synchronized(state) {
                if (state.pending) {
                    @synchronized(lock) { error=ASB_BUSY; }
                    dispatch_semaphore_signal(ready);return;
                }
                state.pending=YES;
            }
            void (^completion)(id)=^(id image) {
                @synchronized(lock) {
                    if (!expired) { screenshot=image;error=image?0:ASB_NOT_READY; }
                }
                @synchronized(state) { state.pending=NO; }
                dispatch_semaphore_signal(ready);
            };
            IMP implementation=[display methodForSelector:selector];
            ((void (*)(id,SEL,void (^)(id)))implementation)(display,selector,completion);
        });
        long timed_out=dispatch_semaphore_wait(ready,dispatch_time(DISPATCH_TIME_NOW,(int64_t)timeout*NSEC_PER_MSEC));
        @synchronized(lock) { expired=YES;if (timed_out) error=ASB_TIMEOUT; }
        if (error) return error;
        CGImageRef image=NULL;
        if ([screenshot isKindOfClass:NSImage.class]) image=[screenshot CGImageForProposedRect:NULL context:nil hints:nil];
        else if (CFGetTypeID((__bridge CFTypeRef)screenshot)==CGImageGetTypeID()) image=(__bridge CGImageRef)screenshot;
        if (!image) return ASB_DATA;
        size_t width=CGImageGetWidth(image),height=CGImageGetHeight(image);
        if (!width || !height || width>7680 || height>4320) return ASB_DATA;
        unsigned char *data=malloc(width*height*4);
        if (!data) return ASB_MEMORY;
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
        CGContextRef bitmap=CGBitmapContextCreate(data,width,height,8,width*4,color,
            kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
        CGColorSpaceRelease(color);
        if (!bitmap) { free(data);return ASB_MEMORY; }
        CGContextDrawImage(bitmap,CGRectMake(0,0,width,height),image);
        reply->magic=ASB_SCREENSHOT_REPLY_MAGIC;reply->version=ASB_SCREENSHOT_VERSION;
        reply->width=(uint32_t)width;reply->height=(uint32_t)height;
        reply->capture_time_ms=asb_mac_milliseconds();reply->frame_seq=reply->capture_time_ms;
        error=asb_capture_pack(data,(uint32_t)width*4,reply,pixels);
        CGContextRelease(bitmap);free(data);return error;
    }
}
