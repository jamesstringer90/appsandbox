#import "agent_overlay.h"
#import <WebKit/WebKit.h>
#import <QuartzCore/QuartzCore.h>
#include "headless_vm.h"
#import <Carbon/Carbon.h>
#include <math.h>

@interface AsbOverlayWebView:WKWebView
@end
@implementation AsbOverlayWebView
- (NSView *)hitTest:(NSPoint)point { (void)point;return nil; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)becomeFirstResponder { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event;return NO; }
- (BOOL)performKeyEquivalent:(NSEvent *)event { (void)event;return NO; }
- (BOOL)canBecomeKeyView { return NO; }
@end

@interface AsbAgentOverlay()<WKScriptMessageHandler,WKNavigationDelegate>
@property(nonatomic,readwrite) BOOL blocksInput;
@end

@implementation AsbAgentOverlay {
    __weak NSView *_view;
    AsbOverlayWebView *_web;
    CAShapeLayer *_mask;
    AsbMacInput *_input;
    AsbMacInputStatus _status;
    NSTimer *_timer;
    id _monitor;
    NSRect _menu;
    uint64_t _identity;
    BOOL _ready,_closed,_hovered,_updating;
}

- (instancetype)initWithView:(NSView *)view vmId:(uint64_t)identity
{
    self=[super init];if (!self) return nil;
    _view=view;_identity=identity;
    AsbMacHeadlessTarget target={0};
    if (asb_mac_target(identity,&target)) {
        _input=asb_mac_input_session(&target);asb_mac_channel_release(target.channel);
    }
    WKWebViewConfiguration *configuration=[WKWebViewConfiguration new];
    WKUserContentController *messages=[WKUserContentController new];
    NSString *bridge=@"window.chrome={webview:{postMessage:s=>window.webkit.messageHandlers.agentOverlay.postMessage(s),addEventListener:(name,fn)=>{if(name==='message')window.__asbOverlayMessage=data=>fn({data});}}};";
    [messages addUserScript:[[WKUserScript alloc] initWithSource:bridge injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
    [messages addScriptMessageHandler:self name:@"agentOverlay"];
    configuration.userContentController=messages;
    _web=[[AsbOverlayWebView alloc] initWithFrame:view.bounds configuration:configuration];
    _web.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    _web.underPageBackgroundColor=NSColor.clearColor;
    _web.wantsLayer=YES;_mask=[CAShapeLayer layer];_mask.fillColor=NSColor.blackColor.CGColor;
    _web.layer.mask=_mask;
    _web.navigationDelegate=self;_web.hidden=YES;
    [view addSubview:_web positioned:NSWindowAbove relativeTo:nil];
    NSURL *directory=[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"web" isDirectory:YES];
    [_web loadFileURL:[directory URLByAppendingPathComponent:@"agent-overlay.html"] allowingReadAccessToURL:directory];
    __weak AsbAgentOverlay *weakSelf=self;
    _timer=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *timer) { (void)timer;[weakSelf refresh]; }];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    NSEventMask mask=NSEventMaskMouseMoved|NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskRightMouseDown|
        NSEventMaskRightMouseUp|NSEventMaskOtherMouseDown|NSEventMaskOtherMouseUp|NSEventMaskLeftMouseDragged|
        NSEventMaskRightMouseDragged|NSEventMaskOtherMouseDragged|NSEventMaskScrollWheel|NSEventMaskKeyDown|
        NSEventMaskKeyUp|NSEventMaskFlagsChanged;
    _monitor=[NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
        return [weakSelf handleEvent:event]?nil:event;
    }];
    return self;
}

- (void)post:(NSDictionary *)message completion:(void (^)(void))completion
{
    if (_closed || !_ready) { if (completion) completion();return; }
    NSData *data=[NSJSONSerialization dataWithJSONObject:message options:0 error:nil];
    NSString *json=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    [_web evaluateJavaScript:[NSString stringWithFormat:@"window.__asbOverlayMessage(%@)",json]
        completionHandler:^(id value,NSError *error) { (void)value;(void)error;if (completion) completion(); }];
}

- (void)refresh
{
    if (_closed) return;
    if (!_input) {
        AsbMacHeadlessTarget target={0};
        if (asb_mac_target(_identity,&target)) {
            _input=asb_mac_input_session(&target);asb_mac_channel_release(target.channel);
        }
    }
    asb_mac_input_status(_input,&_status);
    self.blocksInput=!(_status.flags&ASB_STATUS_USER_HAS_CONTROL);
    BOOL finished=(_status.flags&ASB_STATUS_AGENT_FINISHED)!=0;
    BOOL active=(_status.flags&ASB_STATUS_AGENT_HAS_CONTROL)!=0;
    BOOL menu=!finished && (active || (_status.flags&ASB_STATUS_AGENT_WAITING));
    BOOL cursor=!finished && _status.x<=65535 && _status.y<=65535;
    _web.hidden=!_ready || !(menu || cursor);
    if (_updating || !_ready) return;
    NSSize bounds=_view.bounds.size;double x=0,y=0;
    if (cursor) {
        double width=_status.width?:bounds.width,height=_status.height?:bounds.height;
        double scale=fmin(bounds.width/width,bounds.height/height);
        x=(bounds.width-width*scale)/2+_status.x/65535.0*width*scale;
        y=(bounds.height-height*scale)/2+_status.y/65535.0*height*scale;
    }
    NSDictionary *context=@{@"identity":@(_status.identity),@"vendor":@(_status.vendor),
        @"model":@(_status.model),@"purpose":@(_status.purpose)};
    _updating=YES;__weak AsbAgentOverlay *weakSelf=self;
    [self post:@{@"active":@(active),@"menu":@(menu),@"showCursor":@(cursor),@"x":@(x),@"y":@(y),@"context":context}
        completion:^{ AsbAgentOverlay *strongSelf=weakSelf;if (strongSelf) strongSelf->_updating=NO; }];
}

- (BOOL)handleEvent:(NSEvent *)event
{
    NSView *view=_view;NSWindow *window=view.window;
    if (_closed || !window || event.window!=window) return NO;
    NSResponder *responder=window.firstResponder;
    if ([responder isKindOfClass:NSView.class] && [(NSView *)responder isDescendantOf:_web]) [window makeFirstResponder:view];
    asb_mac_input_status(_input,&_status);
    self.blocksInput=!(_status.flags&ASB_STATUS_USER_HAS_CONTROL);
    NSPoint point=[view convertPoint:window.mouseLocationOutsideOfEventStream fromView:nil];
    BOOL key=event.type==NSEventTypeKeyDown || event.type==NSEventTypeKeyUp || event.type==NSEventTypeFlagsChanged;
    BOOL inside=NSPointInRect(point,view.bounds);
    if (!key && !inside) return NO;
    double x=point.x,y=NSHeight(view.bounds)-point.y;
    BOOL menu=!(_status.flags&ASB_STATUS_AGENT_FINISHED) &&
        (_status.flags&(ASB_STATUS_AGENT_HAS_CONTROL|ASB_STATUS_AGENT_WAITING));
    BOOL hit=menu && _ready && NSPointInRect(NSMakePoint(x,y),_menu);
    if (hit) {
        NSString *kind=@"move";
        BOOL down=event.type==NSEventTypeLeftMouseDown || event.type==NSEventTypeRightMouseDown || event.type==NSEventTypeOtherMouseDown;
        BOOL up=event.type==NSEventTypeLeftMouseUp || event.type==NSEventTypeRightMouseUp || event.type==NSEventTypeOtherMouseUp;
        if (key) kind=@"key";else if (down) kind=@"down";else if (up) kind=@"up";
        else if (event.type==NSEventTypeScrollWheel) kind=@"wheel";
        unsigned code=key?(event.keyCode==kVK_Return?13:event.keyCode==kVK_Space?32:0):0;
        [self post:@{@"type":@"input",@"kind":kind,@"x":@(x),@"y":@(y),@"button":@(key?0:event.buttonNumber),
            @"key":@(code),@"up":@(event.type==NSEventTypeKeyUp),
            @"delta":@(event.type==NSEventTypeScrollWheel?event.scrollingDeltaY:0)} completion:nil];
        _hovered=YES;return YES;
    }
    if (_hovered) { [self post:@{@"type":@"input",@"kind":@"leave"} completion:nil];_hovered=NO; }
    if (self.blocksInput) return YES;
    return NO;
}

- (CGAffineTransform)cssTransform
{
    NSPoint origin=NSMakePoint(0,_web.isFlipped?0:NSHeight(_web.bounds));
    NSPoint x=[_web convertPointToLayer:NSMakePoint(1,origin.y)];
    NSPoint y=[_web convertPointToLayer:NSMakePoint(0,origin.y+(_web.isFlipped?1:-1))];
    origin=[_web convertPointToLayer:origin];
    return CGAffineTransformMake(x.x-origin.x,x.y-origin.y,y.x-origin.x,y.y-origin.y,origin.x,origin.y);
}

- (void)maskLayout:(NSDictionary *)layout
{
    CGMutablePathRef shapes=CGPathCreateMutable();CGAffineTransform transform=[self cssTransform];
    CGPathAddRoundedRect(shapes,&transform,NSRectToCGRect(_menu),10,10);
    if ([layout[@"cursor"] boolValue]) {
        NSString *source=layout[@"cursorPath"];
        if ([source isKindOfClass:NSString.class] && source.length<512) {
            NSScanner *scanner=[NSScanner scannerWithString:source];CGMutablePathRef cursor=CGPathCreateMutable();
            while (!scanner.isAtEnd) {
                NSString *command=nil;double x,y;
                if (![scanner scanCharactersFromSet:NSCharacterSet.letterCharacterSet intoString:&command]) break;
                if ([command isEqualToString:@"Z"]) { CGPathCloseSubpath(cursor);continue; }
                if (!([command isEqualToString:@"M"] || [command isEqualToString:@"L"]) ||
                    ![scanner scanDouble:&x] || ![scanner scanDouble:&y] || !isfinite(x) || !isfinite(y)) break;
                x+=[layout[@"cx"] doubleValue];y+=[layout[@"cy"] doubleValue];
                if ([command isEqualToString:@"M"]) CGPathMoveToPoint(cursor,&transform,x,y);
                else CGPathAddLineToPoint(cursor,&transform,x,y);
            }
            CGPathAddPath(shapes,NULL,cursor);
            CGPathRef outline=CGPathCreateCopyByStrokingPath(cursor,NULL,2,kCGLineCapRound,kCGLineJoinRound,1);
            if (outline) { CGPathAddPath(shapes,NULL,outline);CGPathRelease(outline); }
            CGPathRelease(cursor);
        }
        NSDictionary *label=layout[@"label"];
        if ([label isKindOfClass:NSDictionary.class]) {
            CGRect rectangle=CGRectMake([label[@"x"] doubleValue],[label[@"y"] doubleValue],
                [label[@"width"] doubleValue],[label[@"height"] doubleValue]);
            CGPathAddRoundedRect(shapes,&transform,rectangle,4,4);
        }
    }
    [CATransaction begin];[CATransaction setDisableActions:YES];
    _mask.frame=_web.layer.bounds;_mask.path=shapes;
    [CATransaction commit];CGPathRelease(shapes);
}

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message
{
    (void)controller;
    if (_closed || !message.frameInfo.mainFrame || ![message.body isKindOfClass:NSString.class]) return;
    NSString *command=message.body;
    if ([command isEqualToString:@"ready"]) { _ready=YES;[self refresh];return; }
    if ([command hasPrefix:@"layout:"]) {
        NSData *bytes=[[command substringFromIndex:7] dataUsingEncoding:NSUTF8StringEncoding];
        NSDictionary *layout=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
        if (![layout isKindOfClass:NSDictionary.class]) return;
        NSRect rectangle=NSMakeRect([layout[@"x"] doubleValue],[layout[@"y"] doubleValue],
            [layout[@"width"] doubleValue],[layout[@"height"] doubleValue]);
        if (NSMinX(rectangle)>=0 && NSMinY(rectangle)>=0 && NSMaxX(rectangle)<=NSWidth(_view.bounds)+2 &&
            NSMaxY(rectangle)<=NSHeight(_view.bounds)+2) { _menu=rectangle;[self maskLayout:layout]; }
        return;
    }
    uint32_t control=[command isEqualToString:@"take_control"]?ASB_INPUT_USER_REVOKE:
        [command isEqualToString:@"return_control"]?ASB_INPUT_USER_GRANT:
        [command isEqualToString:@"stop_agent"]?ASB_INPUT_USER_CANCEL:0;
    if (control && _status.work_id) { asb_mac_input_control(_input,control,_status.work_id);[self refresh]; }
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
    (void)webView;
    decisionHandler(action.request.URL.isFileURL?WKNavigationActionPolicyAllow:WKNavigationActionPolicyCancel);
}

- (void)invalidate
{
    if (_closed) return;_closed=YES;
    [_timer invalidate];_timer=nil;
    if (_monitor) [NSEvent removeMonitor:_monitor];_monitor=nil;
    [_web.configuration.userContentController removeScriptMessageHandlerForName:@"agentOverlay"];
    _web.navigationDelegate=nil;[_web removeFromSuperview];_web=nil;
    asb_mac_input_release(_input);_input=NULL;
}

- (void)dealloc { [self invalidate]; }
@end
