#import "fixture-stress.h"
#import <WebKit/WebKit.h>

@interface ASBStressLabel : NSTextField
@property double readDelay;
@end
@implementation ASBStressLabel
- (id)accessibilityValue {
    if (self.readDelay > 0) [NSThread sleepForTimeInterval:self.readDelay];
    return [super accessibilityValue];
}
@end

@interface ASBStressFixtureView () <WKScriptMessageHandler, WKNavigationDelegate>
@property NSTextField *status, *tail;
@property NSView *forest;
@property NSMutableArray<NSView *> *groups;
@property NSMutableArray<NSTextField *> *canaries;
@property WKWebView *web;
@property NSTimer *timer;
@property id activity;
@property NSString *mode;
@property NSUInteger nodes, tick, webTick, pings;
@property double hz, limit, started, finished;
@property BOOL running, webReady;
@end

@implementation ASBStressFixtureView
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityIdentifier { return @"asb-stress"; }
- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    NSArray *titles = @[@"Start updates", @"Stop updates", @"Ping"];
    NSArray *ids = @[@"start", @"stop", @"ping"];
    SEL actions[] = {@selector(start:), @selector(stop:), @selector(ping:)};
    for (NSUInteger i = 0; i < titles.count; i++) {
        NSButton *button = [NSButton buttonWithTitle:titles[i] target:self action:actions[i]];
        button.frame = NSMakeRect(12 + i * 160, 405, 150, 28);
        button.accessibilityIdentifier = [@"asb-stress-" stringByAppendingString:ids[i]];
        [self addSubview:button];
    }
    self.status = [NSTextField labelWithString:@""]; self.status.frame = NSMakeRect(12, 370, 480, 28);
    self.status.accessibilityIdentifier = @"asb-stress-state"; [self addSubview:self.status];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 45, 480, 315)];
    scroll.hasVerticalScroller = YES;
    self.forest = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 300)];
    self.forest.accessibilityElement = YES; self.forest.accessibilityRole = NSAccessibilityGroupRole;
    self.forest.accessibilityIdentifier = @"asb-stress-forest";
    scroll.documentView = self.forest; [self addSubview:scroll];
    self.tail = [NSTextField labelWithString:@"tail:0"]; self.tail.frame = NSMakeRect(12, 10, 480, 28);
    self.tail.accessibilityIdentifier = @"asb-stress-tail"; [self addSubview:self.tail];
    self.mode = @"values"; self.groups = NSMutableArray.array; self.canaries = NSMutableArray.array;
    [self publish]; return self;
}
- (ASBStressLabel *)label:(NSUInteger)index group:(NSUInteger)group generation:(NSUInteger)generation {
    ASBStressLabel *label = [ASBStressLabel labelWithString:[NSString stringWithFormat:@"group:%lu item:%lu tick:%lu",group,index,generation]];
    label.accessibilityIdentifier = [NSString stringWithFormat:@"asb-live-%lu-%lu-%lu",group,index,generation];
    label.frame = NSMakeRect(4, index * 22, 440, 20);
    label.readDelay = [self.mode hasPrefix:@"slow-"] ? .004 : 0;
    return label;
}
- (void)configure:(NSDictionary *)configuration {
    [self stop:nil];
    self.mode = configuration[@"mode"] ?: @"values";
    self.nodes = MIN(4096, MAX(32, [configuration[@"nodes"] unsignedIntegerValue] ?: 256));
    self.hz = MIN(60, MAX(1, [configuration[@"hz"] doubleValue] ?: 20));
    self.limit = MIN(180, MAX(5, [configuration[@"limit"] doubleValue] ?: 60));
    self.tick = self.webTick = self.pings = 0; self.started = self.finished = 0;
    [self.web.configuration.userContentController removeScriptMessageHandlerForName:@"stress"];
    self.web = nil; self.webReady = NO;
    for (NSView *view in self.forest.subviews.copy) [view removeFromSuperview];
    [self.groups removeAllObjects];
    [self.canaries removeAllObjects];
    NSUInteger groupCount = [self.mode isEqual:@"mixed"] ? 3 : [self.mode isEqual:@"web"] ? 0 : 1;
    for (NSUInteger group = 0; group < groupCount; group++) {
        NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, group * self.nodes * 22, 460, self.nodes * 22)];
        view.accessibilityRole = NSAccessibilityGroupRole;
        view.accessibilityIdentifier = [NSString stringWithFormat:@"asb-stress-group-%lu", group];
        for (NSUInteger i = 0; i < self.nodes; i++) [view addSubview:[self label:i group:group generation:0]];
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 450, 22)];
        box.accessibilityElement = YES; box.accessibilityRole = NSAccessibilityGroupRole;
        NSTextField *canary = [NSTextField labelWithString:@"deep:0"];
        canary.frame = box.bounds; canary.accessibilityIdentifier = [NSString stringWithFormat:@"asb-stress-deep-%lu",group];
        [box addSubview:canary]; [view addSubview:box]; [self.canaries addObject:canary];
        [self.forest addSubview:view]; [self.groups addObject:view];
    }
    double height = MAX(300, groupCount * self.nodes * 22);
    if ([@[@"web", @"mixed"] containsObject:self.mode]) {
        WKWebViewConfiguration *config = WKWebViewConfiguration.new;
        [config.userContentController addScriptMessageHandler:self name:@"stress"];
        self.web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 460, 600) configuration:config];
        self.web.navigationDelegate = self;
        [self.forest addSubview:self.web]; height += 600;
        for (NSView *group in self.groups) { NSRect frame = group.frame; frame.origin.y += 600; group.frame = frame; }
        NSString *html = [NSString stringWithFormat:@"<!doctype html><meta charset=utf-8><main aria-label='Updating browser tree'><output id=state></output><div id=rows></div><div id=canary></div></main><script>let tick=0,timer;const n=%lu,hz=%g;"
            "function row(i){let e=document.createElement('button');e.id='live-'+i;e.textContent='row '+i+' tick '+tick;return e;}for(let i=0;i<n;i++)rows.append(row(i));"
            "function publish(){state.textContent='browser tick '+tick;let e=document.createElement('button');e.id='asb-stress-web-deep';e.textContent='deep:'+tick;canary.replaceChildren(e);webkit.messageHandlers.stress.postMessage(tick);}"
            "function start(){clearInterval(timer);timer=setInterval(()=>{tick++;for(let j=0;j<16;j++){let i=(tick*16+j)%%n;document.getElementById('live-'+i).replaceWith(row(i));}publish();},1000/hz);}function stop(){clearInterval(timer);publish();}publish();</script>",self.nodes,self.hz];
        [self.web loadHTMLString:html baseURL:nil];
    }
    self.forest.frame = NSMakeRect(0, 0, 460, height);
    self.tail.stringValue = @"tail:0";
    [self publish]; NSAccessibilityPostNotification(self, NSAccessibilityLayoutChangedNotification);
    if ([configuration[@"autostart"] boolValue]) [self start:nil];
}
- (NSDictionary *)state {
    return @{@"mode":self.mode ?: @"values", @"nodes":@(self.nodes), @"hz":@(self.hz), @"running":@(self.running),
        @"tick":@(self.tick), @"webTick":@(self.webTick), @"pings":@(self.pings),
        @"webReady":@(self.webReady), @"elapsed":@(self.started ? (self.running ? NSProcessInfo.processInfo.systemUptime : self.finished) - self.started : 0)};
}
- (void)publish {
    NSData *data = [NSJSONSerialization dataWithJSONObject:self.state options:NSJSONWritingSortedKeys error:NULL];
    self.status.stringValue = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    NSAccessibilityPostNotification(self.status, NSAccessibilityValueChangedNotification);
}
- (void)start:(id)sender {
    (void)sender; if (self.running) return;
    self.running = YES; self.started = NSProcessInfo.processInfo.systemUptime;
    self.activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"Accessibility stress test"];
    [self.web evaluateJavaScript:@"start()" completionHandler:nil];
    __weak ASBStressFixtureView *weak = self;
    self.timer = [NSTimer timerWithTimeInterval:1 / self.hz repeats:YES block:^(NSTimer *timer) { (void)timer; [weak update]; }];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    [self publish];
}
- (void)update {
    if (!self.running) return;
    if (NSProcessInfo.processInfo.systemUptime - self.started >= self.limit) { [self stop:nil]; return; }
    self.tick++;
    for (NSUInteger group = 0; group < self.groups.count; group++) {
        NSView *view = self.groups[group]; NSArray *children = [view.subviews subarrayWithRange:NSMakeRange(0,self.nodes)];
        BOOL churn = [self.mode hasSuffix:@"churn"] || ([self.mode isEqual:@"mixed"] && group == 1);
        for (NSUInteger j = 0; j < MIN((NSUInteger)32, children.count); j++) {
            NSUInteger index = (self.tick * 32 + j) % children.count;
            NSTextField *old = children[index];
            if (churn) { ASBStressLabel *replacement = [self label:index group:group generation:self.tick]; replacement.frame = old.frame; [view replaceSubview:old with:replacement]; }
            else { old.stringValue = [NSString stringWithFormat:@"group:%lu item:%lu tick:%lu",group,index,self.tick]; NSAccessibilityPostNotification(old, NSAccessibilityValueChangedNotification); }
        }
        NSTextField *canary = self.canaries[group];
        if (churn) {
            NSTextField *replacement = [NSTextField labelWithString:[NSString stringWithFormat:@"deep:%lu",self.tick]];
            replacement.frame = canary.frame; replacement.accessibilityIdentifier = canary.accessibilityIdentifier;
            [canary.superview replaceSubview:canary with:replacement]; self.canaries[group] = replacement;
        } else { canary.stringValue = [NSString stringWithFormat:@"deep:%lu",self.tick]; NSAccessibilityPostNotification(canary, NSAccessibilityValueChangedNotification); }
        if (churn) NSAccessibilityPostNotification(view, NSAccessibilityLayoutChangedNotification);
    }
    self.tail.stringValue = [NSString stringWithFormat:@"tail:%lu",self.tick];
    NSAccessibilityPostNotification(self.tail, NSAccessibilityValueChangedNotification); [self publish];
}
- (void)stop:(id)sender {
    (void)sender; if (self.running) self.finished = NSProcessInfo.processInfo.systemUptime;
    self.running = NO; [self.timer invalidate]; self.timer = nil;
    [self.web evaluateJavaScript:@"stop()" completionHandler:nil];
    if (self.activity) [NSProcessInfo.processInfo endActivity:self.activity]; self.activity = nil;
    [self publish];
}
- (void)ping:(id)sender { (void)sender; self.pings++; [self publish]; }
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation { (void)view; (void)navigation; self.webReady = YES; [self publish]; }
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller; if ([message.body isKindOfClass:NSNumber.class]) self.webTick = [message.body unsignedIntegerValue]; [self publish];
}
@end
