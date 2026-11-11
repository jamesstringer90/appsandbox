#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import "asb_core_mac.h"
#import "vz_display.h"
#import "vz_vm.h"
#import "agent_overlay.h"
#include "headless_vm.h"
#include "../../tools/transport/asb_capture_service.h"
#include <stdlib.h>

@interface AsbCaptureSource:NSObject
@property IOSurfaceRef surface;
@property CGImageRef image;
- (void)releaseSurface;
@end
@implementation AsbCaptureSource
- (void)releaseSurface {
    if (_surface) { IOSurfaceDecrementUseCount(_surface);CFRelease(_surface);_surface=NULL; }
}
- (void)dealloc { [self releaseSurface];if (_image) CGImageRelease(_image); }
@end

static AsbCaptureSource *frame_source(CALayer *layer) {
    if ([layer.name isEqualToString:AsbAgentOverlayLayerName]) return nil;
    id contents=layer.contents;
    if (contents) {
        CFTypeRef value=(__bridge CFTypeRef)contents;
        AsbCaptureSource *source=nil;
        if (CFGetTypeID(value)==IOSurfaceGetTypeID()) {
            source=[AsbCaptureSource new];source.surface=(IOSurfaceRef)CFRetain(value);
            IOSurfaceIncrementUseCount(source.surface);
        } else if (CFGetTypeID(value)==CGImageGetTypeID()) {
            source=[AsbCaptureSource new];source.image=CGImageRetain((CGImageRef)value);
        }
        if (source) return source;
    }
    for (CALayer *child in layer.sublayers) { AsbCaptureSource *source=frame_source(child);if (source) return source; }
    return nil;
}

typedef void (^FrameCopied)(uint32_t,AsbScreenshotReply,unsigned char *);
static void copy_frame(AsbCaptureSource *source,FrameCopied copied) {
    dispatch_assert_queue_not(dispatch_get_main_queue());
    AsbScreenshotReply reply={.magic=ASB_SCREENSHOT_REPLY_MAGIC,.version=ASB_SCREENSHOT_VERSION};
    if (source.image) {
        size_t width=CGImageGetWidth(source.image),height=CGImageGetHeight(source.image);
        if (!width || !height || width>7680 || height>4320) { copied(ASB_DATA,reply,NULL);return; }
        unsigned char *data=calloc(height,width*4);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context=data?CGBitmapContextCreate(data,width,height,8,width*4,color,
            kCGBitmapByteOrder32Little|(CGBitmapInfo)kCGImageAlphaPremultipliedFirst):NULL;
        CGColorSpaceRelease(color);
        if (!context) { free(data);copied(ASB_MEMORY,reply,NULL);return; }
        CGContextDrawImage(context,CGRectMake(0,0,width,height),source.image);CGContextRelease(context);
        reply.width=(uint32_t)width;reply.height=(uint32_t)height;reply.stride=(uint32_t)width*4;
        reply.data_size=reply.stride*reply.height;reply.capture_time_ms=asb_mac_milliseconds();reply.frame_seq=reply.capture_time_ms;
        copied(ASB_OK,reply,data);return;
    }
    IOSurfaceRef surface=source.surface;
    size_t width=IOSurfaceGetWidth(surface),height=IOSurfaceGetHeight(surface);
    OSType format=IOSurfaceGetPixelFormat(surface);
    if (!width || !height || width>7680 || height>4320 || IOSurfaceGetPlaneCount(surface)>1) { copied(ASB_DATA,reply,NULL);return; }
    if (format!='BGRA') { NSLog(@"Native VZ capture unsupported pixel format: 0x%08x",format);copied(ASB_NOT_SUPPORTED,reply,NULL);return; }
    static id<MTLDevice> device;static id<MTLCommandQueue> queue;static dispatch_once_t once;
    dispatch_once(&once,^{ device=MTLCreateSystemDefaultDevice();queue=[device newCommandQueueWithMaxCommandBufferCount:2];queue.label=@"AppSandbox screenshot readback"; });
    MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:desc iosurface:surface plane:0];
    size_t stride=(width*4+255)&~(size_t)255;
    id<MTLBuffer> buffer=[device newBufferWithLength:stride*height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    if (!texture || !buffer || !blit) { if (blit) [blit endEncoding];copied(ASB_NOT_READY,reply,NULL);return; }
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:stride destinationBytesPerImage:stride*height];[blit endEncoding];
    reply.width=(uint32_t)width;reply.height=(uint32_t)height;
    reply.capture_time_ms=asb_mac_milliseconds();reply.frame_seq=reply.capture_time_ms;
    [command addCompletedHandler:^(id<MTLCommandBuffer> complete) {
        [source releaseSurface];
        if (complete.status!=MTLCommandBufferStatusCompleted) { copied(ASB_NOT_READY,reply,NULL);return; }
        AsbScreenshotReply packed=reply;unsigned char *pixels=NULL;
        uint32_t result=asb_capture_pack(buffer.contents,(uint32_t)stride,&packed,&pixels);
        copied(result,packed,pixels);
    }];
    [command commit];
}

@interface AsbFrameCapture:NSObject
@property uint64_t identity,run;
@property BOOL expired,completed,inflight;
@property uint32_t result;
@property dispatch_semaphore_t ready;
@property AsbScreenshotReply reply;
@property unsigned char *pixels;
@property VZVirtualMachineView *view;
@property VzDisplayWindow *offscreen;
- (void)begin;
- (void)poll;
- (void)finish:(uint32_t)result;
@end
static NSMutableDictionary<NSNumber *,AsbFrameCapture *> *captures;

@implementation AsbFrameCapture
- (void)dealloc { free(_pixels); }
- (void)finish:(uint32_t)result {
    dispatch_assert_queue(dispatch_get_main_queue());
    if (self.offscreen) {
        self.view.virtualMachine=nil;self.offscreen.window.contentView=nil;
        [self.offscreen.window close];self.offscreen=nil;
    }
    self.view=nil;
    if (!self.inflight && captures[@(self.identity)]==self) [captures removeObjectForKey:@(self.identity)];
    @synchronized(self) {
        if (self.completed) return;
        self.completed=YES;self.result=self.expired?ASB_TIMEOUT:result;
    }
    dispatch_semaphore_signal(self.ready);
}
- (void)begin {
    @synchronized(self) { if (self.expired) { [self finish:ASB_TIMEOUT];return; } }
    if (!captures) captures=[NSMutableDictionary dictionary];
    if (captures[@(self.identity)]) { [self finish:ASB_BUSY];return; }
    captures[@(self.identity)]=self;[self poll];
}
- (void)poll {
    @synchronized(self) { if (self.completed) return;if (self.expired) { [self finish:ASB_TIMEOUT];return; } }
    AsbVmMac *vm=asb_mac_vm_find_by_id(self.identity);
    if (!vm || vm->run_id!=self.run || !vm->running || vm->shutting_down) { [self finish:ASB_NOT_READY];return; }
    VzDisplayWindow *viewer=vm->display;
    if (!self.view) {
        if (viewer && !viewer.userClosed && viewer.window.visible && !viewer.window.miniaturized &&
            (viewer.window.occlusionState&NSWindowOcclusionStateVisible)) self.view=viewer.vmView;
        else {
            VZVirtualMachine *machine=vm->vz_handle.machine;
            if (!machine) { [self finish:ASB_NOT_READY];return; }
            dispatch_assert_queue(machine.queue);
            VZGraphicsDisplay *display=machine.graphicsDevices.firstObject.displays.firstObject;
            NSSize size=display.sizeInPixels;
            if (size.width<1 || size.height<1 || size.width>7680 || size.height>4320) { [self finish:ASB_DATA];return; }
            self.offscreen=[[VzDisplayWindow alloc] initWithVzVm:vm->vz_handle vmId:self.identity offscreen:YES];
            self.view=self.offscreen.vmView;
            if (!self.view) { [self finish:ASB_NOT_READY];return; }
        }
    } else if (!self.offscreen && (viewer.vmView!=self.view || viewer.userClosed || !viewer.window.visible)) {
        [self finish:ASB_NOT_READY];return;
    }
    @try {
        if (!self.view.layer) self.view.wantsLayer=YES;
        if (self.offscreen) [self.view layoutSubtreeIfNeeded];
        AsbCaptureSource *source=frame_source(self.view.layer);
        if (!source) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW,16*NSEC_PER_MSEC),dispatch_get_main_queue(),^{ [self poll]; });return; }
        self.inflight=YES;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{ @autoreleasepool {
            copy_frame(source,^(uint32_t result,AsbScreenshotReply reply,unsigned char *pixels) {
                dispatch_async(dispatch_get_main_queue(),^{
                    self.inflight=NO;
                    AsbVmMac *current=asb_mac_vm_find_by_id(self.identity);
                    uint32_t finalResult=result;
                    if (!current || current->run_id!=self.run || !current->running || current->shutting_down) finalResult=ASB_NOT_READY;
                    @synchronized(self) {
                        if (self.expired || self.completed || finalResult) free(pixels);
                        else { self.pixels=pixels;self.reply=reply; }
                    }
                    [self finish:finalResult];
                });
            });
        }});
    } @catch(NSException *exception) { NSLog(@"Native VZ frame acquisition: %@",exception);[self finish:ASB_NOT_READY]; }
}
@end

uint32_t asb_mac_native_capture(const AsbMacHeadlessTarget *target,unsigned timeout,
                              AsbScreenshotReply *reply,unsigned char **pixels) {
    *pixels=NULL;if (!timeout || timeout>10000 || NSThread.isMainThread) return ASB_INVALID;
    @autoreleasepool {
        AsbFrameCapture *capture=[AsbFrameCapture new];capture.identity=target->id;capture.run=target->run_id;
        capture.ready=dispatch_semaphore_create(0);dispatch_async(dispatch_get_main_queue(),^{ [capture begin]; });
        long timedOut=dispatch_semaphore_wait(capture.ready,dispatch_time(DISPATCH_TIME_NOW,(int64_t)timeout*NSEC_PER_MSEC));
        uint32_t error;
        @synchronized(capture) {
            capture.expired=timedOut!=0;error=timedOut?ASB_TIMEOUT:capture.result;
            if (!error) { *reply=capture.reply;*pixels=capture.pixels;capture.pixels=NULL; }
        }
        if (timedOut) dispatch_async(dispatch_get_main_queue(),^{ [capture finish:ASB_TIMEOUT]; });
        return error;
    }
}
