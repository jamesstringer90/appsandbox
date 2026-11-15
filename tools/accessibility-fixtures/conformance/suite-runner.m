#import "suite-common.h"
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fnmatch.h>

static id stable(id value, NSMutableDictionary *references) {
    if ([value isKindOfClass:NSDictionary.class]) {
        if ([value[@"type"] isEqual:@"element"]) {
            id key = value[@"ref"]; if (!references[key]) references[key] = @(references.count);
            return @{@"type":@"element", @"ref":references[key], @"identity":value[@"identity"]};
        }
        NSMutableDictionary *result = NSMutableDictionary.dictionary;
        for (id key in value) if (![@[@"elapsedMs", @"operation"] containsObject:key]) result[key] = stable(value[key], references);
        return result;
    }
    if ([value isKindOfClass:NSArray.class]) { NSMutableArray *result = NSMutableArray.array; for (id item in value) [result addObject:stable(item, references)]; return result; }
    return value;
}
id ASBStable(id value) { return stable(value, NSMutableDictionary.dictionary); }
static id markerTypes(id value) {
    if ([value isKindOfClass:NSDictionary.class]) {
        if ([@[@"text-marker", @"text-marker-range"] containsObject:value[@"type"]]) return @{@"type":value[@"type"]};
        NSMutableDictionary *result = NSMutableDictionary.dictionary; for (id key in value) result[key] = markerTypes(value[key]); return result;
    }
    if ([value isKindOfClass:NSArray.class]) { NSMutableArray *result = NSMutableArray.array; for (id item in value) [result addObject:markerTypes(item)]; return result; }
    return value;
}
NSArray *ASBCompared(NSDictionary *test, NSArray *results) {
    NSMutableArray *normalized = ASBStable(results);
    for (NSUInteger i = 0; i < results.count; i++) {
        NSDictionary *source = results[i]; NSMutableDictionary *value = normalized[i][@"value"];
        if ([@[@"attributes", @"actions", @"queries"] containsObject:source[@"operation"][@"kind"]] && [value[@"type"] isEqual:@"array"])
            value[@"value"] = [value[@"value"] sortedArrayUsingComparator:^NSComparisonResult(id a, id b) { return [ASBString(a) compare:ASBString(b)]; }];
    }
    if (test[@"compareResults"]) { NSMutableArray *selected = NSMutableArray.array; for (NSNumber *index in test[@"compareResults"]) [selected addObject:normalized[index.unsignedIntegerValue]]; normalized = selected; }
    if ([test[@"comparison"] isEqual:@"opaque-marker-types"]) return markerTypes(normalized);
    if ([test[@"comparison"] isEqual:@"rtf-except-generator-version"]) for (NSMutableDictionary *operation in normalized) {
        NSMutableDictionary *value = operation[@"value"];
        if ([value[@"type"] isEqual:@"data"]) {
            NSData *data = [[NSData alloc] initWithBase64EncodedString:value[@"value"] options:0];
            NSString *rtf = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
            ASBRequire(rtf != nil, @"Invalid RTF bytes");
            NSRegularExpression *expression = [NSRegularExpression regularExpressionWithPattern:@"\\\\cocoartf[0-9]+\\b" options:0 error:NULL];
            NSTextCheckingResult *match = [expression firstMatchInString:rtf options:0 range:NSMakeRange(0, rtf.length)];
            if (match) rtf = [rtf stringByReplacingCharactersInRange:match.range withString:@"\\cocoartf"];
            value[@"value"] = [[rtf dataUsingEncoding:NSISOLatin1StringEncoding] base64EncodedStringWithOptions:0];
        }
    }
    return normalized;
}
@interface ASBProbe : NSObject
@property NSTask *task;
@property NSFileHandle *input, *output, *errors;
@property NSMutableData *pending;
@property NSString *errorPath;
@property NSUInteger calls;
- (instancetype)initWithPath:(NSString *)path;
- (NSDictionary *)request:(NSDictionary *)request;
- (void)close;
@end
@implementation ASBProbe
- (instancetype)initWithPath:(NSString *)path {
    if (!(self = [super init])) return nil;
    self.task = NSTask.new; self.task.executableURL = [NSURL fileURLWithPath:path]; self.task.arguments = @[@"--serve"];
    NSPipe *input = NSPipe.pipe, *output = NSPipe.pipe; self.input = input.fileHandleForWriting; self.output = output.fileHandleForReading;
    self.task.standardInput = input; self.task.standardOutput = output; self.pending = NSMutableData.data;
    self.errorPath = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createFileAtPath:self.errorPath contents:nil attributes:nil]; self.errors = [NSFileHandle fileHandleForWritingAtPath:self.errorPath]; self.task.standardError = self.errors;
    NSError *error = nil; ASBRequire([self.task launchAndReturnError:&error], error.localizedDescription); return self;
}
- (NSDictionary *)request:(NSDictionary *)request {
    self.calls++; NSMutableData *data = [ASBJSON(request) mutableCopy]; [data appendBytes:"\n" length:1]; [self.input writeData:data];
    double deadline = NSProcessInfo.processInfo.systemUptime + (request[@"budgetSeconds"] ? [request[@"budgetSeconds"] doubleValue] : 20) + 2;
    for (;;) {
        const unsigned char *bytes = self.pending.bytes;
        for (NSUInteger i = 0; i < self.pending.length; i++) if (bytes[i] == '\n') {
            NSData *line = [self.pending subdataWithRange:NSMakeRange(0, i)];
            [self.pending replaceBytesInRange:NSMakeRange(0, i + 1) withBytes:NULL length:0]; return ASBParse(line);
        }
        double remaining = deadline - NSProcessInfo.processInfo.systemUptime;
        if (remaining <= 0) { kill(self.task.processIdentifier, SIGKILL); ASBRequire(NO, @"Probe deadline"); }
        struct pollfd descriptor = {.fd=self.output.fileDescriptor, .events=POLLIN};
        int ready = poll(&descriptor, 1, (int)(remaining * 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) kill(self.task.processIdentifier, SIGKILL);
        ASBRequire(ready > 0, @"Probe read deadline");
        unsigned char buffer[16384]; ssize_t count = read(descriptor.fd, buffer, sizeof(buffer));
        ASBRequire(count > 0, [@"Probe exited: " stringByAppendingString:[NSString stringWithContentsOfFile:self.errorPath encoding:NSUTF8StringEncoding error:NULL] ?: @""]);
        [self.pending appendBytes:buffer length:(NSUInteger)count];
        ASBRequire(self.pending.length <= 64 * 1024 * 1024, @"Probe response limit");
    }
}
- (void)close {
    [self.input closeFile]; double deadline = NSProcessInfo.processInfo.systemUptime + 2;
    while (self.task.running && NSProcessInfo.processInfo.systemUptime < deadline) [NSThread sleepForTimeInterval:.01];
    if (self.task.running) kill(self.task.processIdentifier, SIGKILL);
    [self.task waitUntilExit]; [self.output closeFile]; [self.errors closeFile]; [NSFileManager.defaultManager removeItemAtPath:self.errorPath error:NULL];
}
@end
@interface ASBEndpoint : NSObject
@property ASBProbe *probe;
@property NSNumber *pid;
@property NSString *window, *under, *stateAttribute;
@property NSArray *excludedSubtrees;
@property NSMutableDictionary *handles;
@property NSMutableArray *trace;
- (NSDictionary *)call:(id)selector operations:(NSArray *)operations extra:(NSDictionary *)extra;
- (NSDictionary *)state;
- (void)reset:(NSString *)token;
- (void)clear;
@end
@implementation ASBEndpoint
- (instancetype)init { if ((self = [super init])) { self.handles = NSMutableDictionary.dictionary; self.trace = NSMutableArray.array; self.stateAttribute = @"AXValue"; } return self; }
- (NSDictionary *)call:(id)target operations:(NSArray *)operations extra:(NSDictionary *)extra {
    NSDictionary *selector = [target isKindOfClass:NSDictionary.class] ? target : @{@"AXIdentifier":target}; NSString *key = ASBString(selector);
    NSMutableDictionary *request = [@{@"pid":self.pid, @"window":self.window, @"operations":operations, @"budgetSeconds":@12} mutableCopy];
    if (self.excludedSubtrees) request[@"excludeSubtrees"] = self.excludedSubtrees;
    [request addEntriesFromDictionary:extra ?: @{}];
    if (self.handles[key]) request[@"handle"] = self.handles[key];
    else { request[@"target"] = selector; request[@"retain"] = @YES; request[@"retainIndex"] = @YES; if (self.under) request[@"under"] = @{@"AXIdentifier":self.under}; }
    NSDictionary *result;
    double until = NSProcessInfo.processInfo.systemUptime + 1;
    for (;;) {
        result = [self.probe request:request]; [self.trace addObject:result];
        BOOL missing = !request[@"handle"] && [result[@"matches"] isEqual:@0] && ![result[@"lookupIncomplete"] boolValue] && [result[@"issues"] isEqual:@[@"target unavailable or ambiguous"]];
        if (!missing || NSProcessInfo.processInfo.systemUptime >= until) break;
        [NSThread sleepForTimeInterval:.02];
    }
    for (NSDictionary *indexed in result[@"indexed"]) self.handles[ASBString(indexed[@"selector"])] = indexed[@"handle"];
    if (result[@"handle"]) self.handles[key] = result[@"handle"];
    ASBRequire([result[@"complete"] boolValue], ASBString(result[@"issues"] ?: result)); return result;
}
- (void)clear { [self.handles removeAllObjects]; [self.probe request:@{@"pid":self.pid, @"scope":@"application", @"operations":@[], @"clearHandles":@YES}]; }
- (NSDictionary *)state {
    NSDictionary *result = [self call:@"asb-state" operations:@[@{@"kind":@"get", @"name":self.stateAttribute}] extra:nil][@"results"][0];
    ASBRequire([result[@"error"] intValue] == 0, [@"State read failed: " stringByAppendingString:ASBString(result)]);
    return ASBParse([result[@"value"][@"value"] dataUsingEncoding:NSUTF8StringEncoding]);
}
- (void)reset:(NSString *)token {
    NSArray *steps = @[@[@"asb-case", @{@"kind":@"set", @"name":@"AXValue", @"value":@{@"type":@"string", @"value":token}}], @[@"asb-reset", @{@"kind":@"action", @"name":@"AXPress"}]];
    for (NSArray *step in steps) { NSDictionary *reply = [self call:step[0] operations:@[step[1]] extra:nil]; ASBRequire([reply[@"results"][0][@"error"] intValue] == 0, @"Fixture reset failed"); }
    [self clear];
}
@end
static NSArray *paired(NSArray<ASBEndpoint *> *endpoints, id (^action)(ASBEndpoint *)) {
    NSMutableArray *results = NSMutableArray.array, *errors = NSMutableArray.array;
    for (NSUInteger i = 0; i < endpoints.count; i++) { [results addObject:NSNull.null]; [errors addObject:NSNull.null]; }
    dispatch_group_t group = dispatch_group_create();
    for (NSUInteger i = 0; i < endpoints.count; i++) dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { @try { id result = action(endpoints[i]) ?: NSNull.null; @synchronized(results) { results[i] = result; } } @catch (NSException *error) { @synchronized(results) { errors[i] = error; } } }
    });
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    for (id error in errors) if (error != NSNull.null) @throw error;
    return results;
}
static NSDictionary *settled(NSArray *endpoints, NSDictionary *expected, double timeout, NSArray *revisions) {
    double start = NSProcessInfo.processInfo.systemUptime; NSMutableArray *attempts = NSMutableArray.array;
    for (;;) {
        NSArray *states = paired(endpoints, ^id(ASBEndpoint *endpoint) { return endpoint.state; }); NSMutableArray *errors = NSMutableArray.array;
        for (NSUInteger i = 0; i < states.count; i++) {
            NSDictionary *state = states[i]; NSMutableDictionary *projected = NSMutableDictionary.dictionary;
            for (id key in expected) projected[key] = state[key] ?: NSNull.null;
            for (NSDictionary *difference in ASBDifferences(expected, projected, @"")) { NSMutableDictionary *entry = difference.mutableCopy; entry[@"side"] = @(i); [errors addObject:entry]; }
            if (revisions && [state[@"revision"] longLongValue] <= [revisions[i] longLongValue]) [errors addObject:@{@"side":@(i), @"path":@"/revision", @"expected":@"advance", @"actual":state[@"revision"] ?: NSNull.null}];
        }
        double elapsed = NSProcessInfo.processInfo.systemUptime - start; [attempts addObject:@{@"elapsed":@(elapsed), @"states":states}];
        if (!errors.count || elapsed >= timeout) return @{@"states":states, @"observations":attempts, @"errors":errors};
        [NSThread sleepForTimeInterval:.02];
    }
}
static BOOL hasError(NSArray *replies) { for (NSDictionary *reply in replies) for (NSDictionary *result in reply[@"results"]) if ([result[@"error"] intValue]) return YES; return NO; }
static BOOL interruptedOperation(NSArray *replies) {
    for (NSDictionary *reply in replies) for (NSDictionary *result in reply[@"results"])
        if ([@[@(-25200),@(-25202),@(-25204)] containsObject:result[@"error"]]) return YES;
    return NO;
}
static NSString *nativeErrorStatus(NSArray *replies) {
    NSMutableSet *errors = NSMutableSet.set;
    for (NSDictionary *reply in replies) for (NSDictionary *result in reply[@"results"]) if ([result[@"error"] intValue]) [errors addObject:result[@"error"]];
    if ([errors containsObject:@(-25201)]) return @"NATIVE_REJECTED";
    if ([errors containsObject:@(-25212)]) return @"NATIVE_NO_VALUE";
    if ([errors isSubsetOfSet:[NSSet setWithArray:@[@(-25205), @(-25206), @(-25208), @(-25213)]]]) return @"NATIVE_UNSUPPORTED";
    return @"INCOMPLETE";
}
static NSArray *parity(NSArray *replies, NSDictionary *test, NSString *path) {
    return replies.count == 2 ? ASBResultDifferences(ASBCompared(test,replies[0][@"results"]),ASBCompared(test,replies[1][@"results"]),path) : @[];
}
static NSDictionary *stressRead(ASBEndpoint *endpoint) {
    NSDictionary *reply = [endpoint call:@"asb-stress-state" operations:@[@{@"kind":@"get",@"name":@"AXValue"}]
        extra:@{@"budgetSeconds":@2,@"messagingTimeout":@0.5}];
    NSDictionary *value = reply[@"results"][0];
    ASBRequire([value[@"error"] intValue] == 0 && [value[@"value"][@"type"] isEqual:@"string"],ASBString(value));
    return ASBParse([value[@"value"][@"value"] dataUsingEncoding:NSUTF8StringEncoding]);
}
static void stressAction(ASBEndpoint *endpoint, NSString *name) {
    NSDictionary *reply = [endpoint call:[@"asb-stress-" stringByAppendingString:name] operations:@[@{@"kind":@"action",@"name":@"AXPress"}]
        extra:@{@"budgetSeconds":@2,@"messagingTimeout":@1}];
    ASBRequire([reply[@"results"][0][@"error"] intValue] == 0,ASBString(reply[@"results"]));
}
static NSDictionary *stressObserve(ASBEndpoint *endpoint, NSDictionary *test, double duration, NSString *reader) {
    NSMutableArray *samples = NSMutableArray.array, *failures = NSMutableArray.array, *cold = NSMutableArray.array;
    NSMutableArray *latencies = NSMutableArray.array, *actionTimes = NSMutableArray.array;
    double start = NSProcessInfo.processInfo.systemUptime, nextPing = start + 1, lastAdvance = start, lastWebAdvance = start;
    double maxGap = 0, maxWebGap = 0; NSUInteger lastTick = 0, lastWebTick = 0, issued = 0;
    BOOL browser = [@[@"web",@"mixed"] containsObject:test[@"mode"]], observedRunning = NO;
    double firstLiveRead = -1;
    dispatch_group_t capture = dispatch_group_create();
    dispatch_group_async(capture, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        ASBProbe *probe = nil;
        @try {
            probe = [[ASBProbe alloc] initWithPath:reader];
            while (NSProcessInfo.processInfo.systemUptime < start + duration) { @autoreleasepool {
                double began = NSProcessInfo.processInfo.systemUptime;
                BOOL web = [test[@"mode"] isEqual:@"web"];
                NSDictionary *target = web ? @{@"AXDOMIdentifier":@"asb-stress-web-deep"} : @{@"AXIdentifier":[test[@"mode"] isEqual:@"mixed"] ? @"asb-stress-deep-1" : @"asb-stress-deep-0"};
                NSMutableDictionary *request = [@{@"pid":endpoint.pid,@"window":endpoint.window,@"target":target,
                    @"operations":@[@{@"kind":@"get",@"name":web ? @"AXTitle" : @"AXValue"}],@"budgetSeconds":@3,@"messagingTimeout":@0.5} mutableCopy];
                if (endpoint.under) request[@"under"] = @{@"AXIdentifier":endpoint.under};
                NSDictionary *reply = [probe request:request];
                [cold addObject:@{@"at":@(began-start),@"seconds":@(NSProcessInfo.processInfo.systemUptime-began),@"reply":reply}];
                double wait = 1 - (NSProcessInfo.processInfo.systemUptime - began);
                if (wait > 0) [NSThread sleepForTimeInterval:wait];
            }}
        } @catch (NSException *failure) { [cold addObject:@{@"at":@(NSProcessInfo.processInfo.systemUptime-start),@"error":failure.reason ?: failure.name}]; }
        @finally { [probe close]; }
    });
    @try {
        while (NSProcessInfo.processInfo.systemUptime < start + duration) { @autoreleasepool {
            double began = NSProcessInfo.processInfo.systemUptime;
            @try {
                NSDictionary *state = stressRead(endpoint); double now = NSProcessInfo.processInfo.systemUptime;
                [latencies addObject:@(now-began)];
                [samples addObject:@{@"at":@(now-start),@"seconds":@(now-began),@"state":state}];
                NSUInteger tick = [state[@"tick"] unsignedIntegerValue], webTick = [state[@"webTick"] unsignedIntegerValue];
                if (tick < lastTick || webTick < lastWebTick) [failures addObject:@"Sequence moved backwards"];
                if (tick > lastTick) { maxGap = MAX(maxGap,now-lastAdvance); lastAdvance = now; lastTick = tick; }
                if (webTick > lastWebTick) { maxWebGap = MAX(maxWebGap,now-lastWebAdvance); lastWebAdvance = now; lastWebTick = webTick; }
                if ([state[@"running"] boolValue]) { if (!observedRunning) firstLiveRead = now-start; observedRunning = YES; }
                else if (observedRunning) [failures addObject:@"Workload stopped during measurement"];
            } @catch (NSException *failure) { [samples addObject:@{@"at":@(NSProcessInfo.processInfo.systemUptime-start),@"error":failure.reason ?: failure.name}]; [failures addObject:@"State read failed"]; }
            if (NSProcessInfo.processInfo.systemUptime >= nextPing) {
                double beganAction = NSProcessInfo.processInfo.systemUptime; issued++;
                @try { stressAction(endpoint,@"ping"); [actionTimes addObject:@(NSProcessInfo.processInfo.systemUptime-beganAction)]; }
                @catch (NSException *failure) { [failures addObject:[@"Action failed: " stringByAppendingString:failure.reason ?: failure.name]]; }
                nextPing = NSProcessInfo.processInfo.systemUptime + 1;
            }
            double wait = .1 - (NSProcessInfo.processInfo.systemUptime-began);
            if (wait > 0) [NSThread sleepForTimeInterval:wait];
        }}
    } @finally {
        maxGap = MAX(maxGap,NSProcessInfo.processInfo.systemUptime-lastAdvance);
        if (browser) maxWebGap = MAX(maxWebGap,NSProcessInfo.processInfo.systemUptime-lastWebAdvance);
        if (!endpoint.probe.task.running) {
            [endpoint.probe close]; endpoint.probe = [[ASBProbe alloc] initWithPath:reader]; [endpoint.handles removeAllObjects];
        }
        @try { stressAction(endpoint,@"stop"); } @catch (NSException *failure) { [failures addObject:[@"Stop failed: " stringByAppendingString:failure.reason ?: failure.name]]; }
        dispatch_group_wait(capture,DISPATCH_TIME_FOREVER);
    }
    double recoveryStart = NSProcessInfo.processInfo.systemUptime; NSDictionary *final = nil;
    while (NSProcessInfo.processInfo.systemUptime-recoveryStart < 12) {
        @try { final = stressRead(endpoint); if (![final[@"running"] boolValue] && [final[@"pings"] unsignedIntegerValue] == issued) break; }
        @catch (NSException *failure) { (void)failure; }
        [NSThread sleepForTimeInterval:.05];
    }
    if (!final || [final[@"running"] boolValue] || [final[@"pings"] unsignedIntegerValue] != issued) [failures addObject:@"Final state did not recover with every action acknowledged"];
    [latencies sortUsingSelector:@selector(compare:)]; [actionTimes sortUsingSelector:@selector(compare:)];
    double maximumRead = [latencies.lastObject doubleValue], maximumAction = [actionTimes.lastObject doubleValue];
    if (!observedRunning || !lastTick || maxGap > 2 || (browser && (!lastWebTick || maxWebGap > 2))) [failures addObject:@"Fresh updates stalled for more than 2 seconds"];
    if (maximumRead > 2 || maximumAction > 2) [failures addObject:@"Read or action exceeded 2 seconds"];
    NSUInteger completeWalks = 0, advancingWalks = 0, lastWalkTick = 0; double lastWalk = 0, maxWalkGap = 0;
    for (NSDictionary *walk in cold) if ([walk[@"reply"][@"complete"] boolValue] && [walk[@"reply"][@"results"][0][@"error"] intValue] == 0) {
        double at = [walk[@"at"] doubleValue] + [walk[@"seconds"] doubleValue];
        if (at > duration) continue;
        completeWalks++;
        NSString *tail = walk[@"reply"][@"results"][0][@"value"][@"value"];
        NSUInteger tick = [tail isKindOfClass:NSString.class] && [tail hasPrefix:@"deep:"] ? [[tail substringFromIndex:5] integerValue] : 0;
        if (tick > lastWalkTick) { advancingWalks++; maxWalkGap = MAX(maxWalkGap,at-lastWalk); lastWalk = at; lastWalkTick = tick; }
    }
    maxWalkGap = MAX(maxWalkGap,duration-lastWalk);
    if (!advancingWalks || maxWalkGap > 5) [failures addObject:@"No complete fresh traversal within 5 seconds"];
    return @{@"status":failures.count ? @"FAIL" : @"PASS",@"failures":failures,@"samples":samples,@"coldTraversals":cold,
        @"metrics":@{@"reads":@(latencies.count),@"maxReadSeconds":@(maximumRead),@"p95ReadSeconds":latencies.count ? latencies[MIN(latencies.count-1,(NSUInteger)(.95*latencies.count))] : NSNull.null,
            @"actionsIssued":@(issued),@"maxActionSeconds":@(maximumAction),@"maxUpdateGapSeconds":@(maxGap),@"maxWebUpdateGapSeconds":@(maxWebGap),
            @"firstLiveReadSeconds":@(firstLiveRead),@"producerHz":[final[@"elapsed"] doubleValue] > 0 ? @([final[@"tick"] doubleValue]/[final[@"elapsed"] doubleValue]) : @0,
            @"webProducerHz":[final[@"elapsed"] doubleValue] > 0 ? @([final[@"webTick"] doubleValue]/[final[@"elapsed"] doubleValue]) : @0,
            @"completeTraversals":@(completeWalks),@"advancingTraversals":@(advancingWalks),@"attemptedTraversals":@(cold.count),@"maxTraversalGapSeconds":@(maxWalkGap),
            @"recoverySeconds":@(NSProcessInfo.processInfo.systemUptime-recoveryStart)},@"finalState":final ?: (id)NSNull.null};
}
int ASBStressRun(NSDictionary *options) {
    ASBAPI *api = ASBAPI.new; NSDictionary *vm = [api status:ASBRequired(options,@"vm")];
    ASBRequire([vm[@"running"] boolValue] && [vm[@"displayOpen"] boolValue],@"The VM and production viewer must be running");
    NSString *output = ASBRequired(options,@"output"), *reader = [ASBDirectory stringByAppendingPathComponent:@"build/ax-probe"];
    ASBRequire(![NSFileManager.defaultManager fileExistsAtPath:output],@"Output directory already exists");
    [NSFileManager.defaultManager createDirectoryAtPath:output withIntermediateDirectories:YES attributes:nil error:NULL];
    double duration = options[@"duration"] ? [options[@"duration"] doubleValue] : 20;
    ASBRequire(duration >= 5 && duration <= 90,@"Stress duration must be 5–90 seconds per workload");
    BOOL pairedWindow = options[@"paired-pid"] != nil;
    ASBEndpoint *left = ASBEndpoint.new, *right = ASBEndpoint.new;
    left.pid = @([ASBRequired(options,pairedWindow ? @"paired-pid" : @"native-pid") intValue]); right.pid = api.discovery[@"pid"];
    ASBRequire(!pairedWindow || [left.pid isEqual:right.pid],@"Paired PID must belong to the production host");
    right.window = options[@"window"] ?: [vm[@"name"] stringByAppendingString:@" — Display"];
    left.window = pairedWindow ? right.window : @"AppSandbox AX Conformance";
    left.under = pairedWindow ? @"asb-native-pane" : nil;
    right.under = options[@"proxy-under"] ?: (pairedWindow ? @"asb-remote-pane" : nil);
    left.excludedSubtrees = right.excludedSubtrees = @[@"asb-stress-forest"];
    NSArray *endpoints = @[left,right]; NSMutableArray *results = NSMutableArray.array;
    NSArray *manifest = ASBLoad([ASBDirectory stringByAppendingPathComponent:@"stress-cases.json"]);
    ASBSave([output stringByAppendingPathComponent:@"provenance.json"],@{@"vm":vm,@"hostPID":right.pid,@"nativePID":left.pid,@"fixtureRevision":ASBFixtureRevision(),@"readerSHA256":ASBHash(reader),
        @"runnerSHA256":ASBHash([ASBDirectory stringByAppendingPathComponent:@"build/ax-suite"]),@"workloads":manifest,@"duration":@(duration),
        @"scope":@"Public AX only. Control lookups exclude the workload forest; independent cold traversals include it and require advancing deep markers while fixture timers continue."});
    @try {
        left.probe = [[ASBProbe alloc] initWithPath:reader]; right.probe = [[ASBProbe alloc] initWithPath:reader];
        NSArray *states = paired(endpoints,^id(ASBEndpoint *endpoint) { return endpoint.state; });
        for (NSDictionary *state in states) ASBRequire([state[@"fixtureRevision"] isEqual:ASBFixtureRevision()],@"Native and guest stress fixture builds must match current sources");
        ASBSave([output stringByAppendingPathComponent:@"preflight.json"],states);
        for (NSDictionary *test in manifest) {
            BOOL selected = NO; for (NSString *pattern in [(options[@"cases"] ?: @"*") componentsSeparatedByString:@","])
                if (fnmatch(pattern.UTF8String,[test[@"id"] UTF8String],0) == 0) selected = YES;
            if (!selected) continue;
            NSMutableDictionary *result = [@{@"id":test[@"id"],@"status":@"INCOMPLETE"} mutableCopy];
            left.trace = NSMutableArray.array; right.trace = NSMutableArray.array;
            @try {
                NSMutableDictionary *config = test.mutableCopy; config[@"limit"] = @(duration+60); config[@"autostart"] = test[@"coldStart"] ?: @NO;
                NSString *token = ASBString(@{@"case":NSUUID.UUID.UUIDString,@"page":@"Stress",@"stress":config});
                paired(endpoints,^id(ASBEndpoint *endpoint) { [endpoint reset:token]; return nil; });
                double setupStart = NSProcessInfo.processInfo.systemUptime;
                NSArray *ready = paired(endpoints,^id(ASBEndpoint *endpoint) {
                    double deadline = NSProcessInfo.processInfo.systemUptime+30;
                    NSDictionary *prepared = nil;
                    for (;;) {
                        @try {
                            NSDictionary *state = stressRead(endpoint);
                            if ([state[@"mode"] isEqual:test[@"mode"]] && [state[@"nodes"] isEqual:test[@"nodes"]] &&
                                (![@[@"web",@"mixed"] containsObject:test[@"mode"]] || [state[@"webReady"] boolValue])) { prepared = state; break; }
                        } @catch (NSException *failure) { if (NSProcessInfo.processInfo.systemUptime >= deadline) @throw failure; [endpoint clear]; }
                        ASBRequire(NSProcessInfo.processInfo.systemUptime < deadline,@"Stress workload did not become ready"); [NSThread sleepForTimeInterval:.1];
                    }
                    for (NSString *name in @[@"start",@"stop",@"ping"]) [endpoint call:[@"asb-stress-" stringByAppendingString:name] operations:@[] extra:nil];
                    ASBRequire(![test[@"coldStart"] boolValue] || [prepared[@"running"] boolValue],@"Cold-start workload stopped before the tree became usable");
                    return @{@"seconds":@(NSProcessInfo.processInfo.systemUptime-setupStart),@"state":prepared};
                });
                result[@"ready"] = ready;
                paired(endpoints,^id(ASBEndpoint *endpoint) { stressAction(endpoint,@"start"); return nil; });
                NSArray *observed = paired(endpoints,^id(ASBEndpoint *endpoint) { return stressObserve(endpoint,test,duration,reader); });
                NSMutableArray *qualified = NSMutableArray.array;
                for (NSUInteger side = 0; side < observed.count; side++) {
                    NSMutableDictionary *outcome = [observed[side] mutableCopy];
                    if ([test[@"coldStart"] boolValue] && [ready[side][@"seconds"] doubleValue] > 5) {
                        outcome[@"failures"] = [outcome[@"failures"] arrayByAddingObject:@"First usable tree exceeded 5 seconds while updates were already running"];
                        outcome[@"status"] = @"FAIL";
                    }
                    [qualified addObject:outcome];
                }
                result[@"native"] = qualified[0]; result[@"proxy"] = qualified[1];
                result[@"status"] = ![qualified[0][@"status"] isEqual:@"PASS"] ? @"BASELINE_FAILED" : qualified[1][@"status"];
            } @catch (NSException *failure) { result[@"reason"] = failure.reason ?: failure.name; }
            @finally { for (ASBEndpoint *endpoint in endpoints) @try { stressAction(endpoint,@"stop"); } @catch (NSException *failure) { (void)failure; } }
            [results addObject:result];
            ASBSave([output stringByAppendingPathComponent:[test[@"id"] stringByAppendingString:@".json"]],@{@"result":result,@"nativeTrace":left.trace,@"proxyTrace":right.trace});
            ASBSave([output stringByAppendingPathComponent:@"result.json"],@{@"schema":@"asb-ax-stress-1",@"cases":results});
            puts(ASBString(@{@"id":test[@"id"],@"status":result[@"status"],@"native":result[@"native"][@"metrics"] ?: (id)NSNull.null,@"proxy":result[@"proxy"][@"metrics"] ?: (id)NSNull.null}).UTF8String);
        }
    } @finally { [left.probe close]; [right.probe close]; }
    ASBRequire(results.count > 0,@"No stress workloads selected");
    for (NSDictionary *result in results) if (![result[@"status"] isEqual:@"PASS"]) return 1;
    return 0;
}
int ASBRun(NSDictionary *options) {
    BOOL nativeOnly = [options[@"native-only"] boolValue];
    NSString *successful = nativeOnly ? @"NATIVE_OK" : @"PASS";
    ASBAPI *api = ASBAPI.new; NSDictionary *vm = [api status:ASBRequired(options, @"vm")]; NSNumber *hostPID = api.discovery[@"pid"];
    ASBRequire([vm[@"running"] boolValue] && [vm[@"displayOpen"] boolValue], @"Open the running VM viewer once before testing; the runner does not activate windows.");
    BOOL pairedWindow = options[@"paired-pid"] != nil;
    NSNumber *pid = @([ASBRequired(options, pairedWindow ? @"paired-pid" : @"native-pid") intValue]);
    ASBRequire(!pairedWindow || [pid isEqual:hostPID], @"Paired window must belong to the active production host");
    NSArray *patterns = [(options[@"cases"] ?: @"*") componentsSeparatedByString:@","]; NSMutableArray *selected = NSMutableArray.array;
    NSMutableSet *failed = nil;
    if (options[@"failed-from"]) { failed = NSMutableSet.set; for (NSDictionary *result in ASBLoad([options[@"failed-from"] stringByAppendingPathComponent:@"result.json"])[@"cases"]) if (![@[successful, @"NOT_APPLICABLE"] containsObject:result[@"status"]]) [failed addObject:result[@"id"]]; }
    for (NSDictionary *test in ASBCases()) for (NSString *pattern in patterns) if (fnmatch(pattern.UTF8String, [test[@"id"] UTF8String], 0) == 0 && (!failed || [failed containsObject:test[@"id"]])) { [selected addObject:test]; break; }
    ASBRequire(selected.count > 0, @"No selected cases");
    NSInteger repeat = options[@"repeat"] ? [options[@"repeat"] integerValue] : 1; double timeout = options[@"timeout"] ? [options[@"timeout"] doubleValue] : 8;
    ASBRequire(repeat >= 1 && repeat <= 1000 && timeout > 0 && timeout <= 120, @"Invalid repeat count or timeout");
    NSString *output = ASBRequired(options, @"output"), *reader = [ASBDirectory stringByAppendingPathComponent:@"build/ax-probe"];
    ASBRequire(![NSFileManager.defaultManager fileExistsAtPath:output], @"Output directory already exists");
    NSError *error = nil; ASBRequire([NSFileManager.defaultManager createDirectoryAtPath:output withIntermediateDirectories:YES attributes:nil error:&error], error.localizedDescription);
    ASBSave([output stringByAppendingPathComponent:@"cases.json"], selected);
    ASBSave([output stringByAppendingPathComponent:@"provenance.json"], @{@"vm":vm, @"hostPID":hostPID, @"nativePID":pid, @"pairedWindow":@(pairedWindow), @"readerSHA256":ASBHash(reader), @"runnerSHA256":ASBHash([ASBDirectory stringByAppendingPathComponent:@"build/ax-suite"]), @"proxyUnder":options[@"proxy-under"] ?: (pairedWindow ? @"asb-remote-pane" : (id)NSNull.null), @"caseSHA256":ASBHash([ASBDirectory stringByAppendingPathComponent:@"cases.json"]), @"catalogSHA256":ASBHash([ASBDirectory stringByAppendingPathComponent:@"macos-sdk.json"]), @"scope":@"Public host AX calls; no screenshots, synthesized input, window activation or per-case VM restart."});
    ASBEndpoint *left = ASBEndpoint.new, *right = ASBEndpoint.new; NSArray *endpoints = nativeOnly ? @[left] : @[left, right];
    NSString *window = options[@"window"] ?: [vm[@"name"] stringByAppendingString:@" — Display"];
    left.pid = pid; left.window = pairedWindow ? window : @"AppSandbox AX Conformance"; left.under = pairedWindow ? @"asb-native-pane" : nil;
    right.pid = hostPID; right.window = window; right.under = options[@"proxy-under"] ?: (pairedWindow ? @"asb-remote-pane" : nil);
    if ([vm[@"osType"] isEqual:@"Windows"]) right.stateAttribute = @"AXDescription";
    NSMutableArray *results = NSMutableArray.array; NSString *epoch = nil, *previousPage = nil; BOOL previousMutation = YES; id previousProvider = nil, frontmost = nil;
    double started = NSProcessInfo.processInfo.systemUptime, checkpointAt = started;
    @try {
        left.probe = [[ASBProbe alloc] initWithPath:reader]; if (!nativeOnly) right.probe = [[ASBProbe alloc] initWithPath:reader];
        NSArray *fixtureStates = paired(endpoints, ^id(ASBEndpoint *endpoint) { return endpoint.state; });
        NSString *fixtureRevision = ASBLoad([ASBDirectory stringByAppendingPathComponent:@"build/fixture-revision.json"])[@"sha256"];
        ASBSave([output stringByAppendingPathComponent:@"preflight.json"],@{@"fixtureRevision":fixtureRevision,@"states":fixtureStates});
        for (NSDictionary *state in fixtureStates) ASBRequire([state[@"fixtureRevision"] isEqual:fixtureRevision],@"Fixture builds differ; rebuild and launch matching native and guest fixtures before testing");
        BOOL stop = NO;
        for (NSInteger repetition = 0; repetition < repeat && !stop; repetition++) for (NSDictionary *test in selected) { @autoreleasepool {
            NSMutableDictionary *item = [@{@"id":test[@"id"], @"caseSHA256":ASBHashValue(test), @"repetition":@(repetition), @"verification":nativeOnly ? @"native-baseline" : @"production-proxy", @"status":@"INCOMPLETE"} mutableCopy];
            if ([test[@"desktop"] isEqual:@"focus"] && ![options[@"include-focus"] boolValue]) { item[@"status"] = @"REQUIRES_FOCUS"; [results addObject:item]; continue; }
            if (!nativeOnly && [vm[@"osType"] isEqual:@"Windows"] && test[@"windows"] == NSNull.null) { item[@"status"] = @"MAPPING_UNASSESSED"; [results addObject:item]; continue; }
            double caseStart = NSProcessInfo.processInfo.systemUptime; left.trace = NSMutableArray.array; right.trace = NSMutableArray.array;
            @try {
                NSString *page = test[@"page"] ?: @"Basics"; BOOL pageChanged = ![page isEqual:previousPage];
                BOOL reset = !epoch || previousMutation || [test[@"mutation"] boolValue] || pageChanged || !ASBEqual(previousProvider, test[@"provider"]);
                if (reset) {
                    epoch = test[@"provider"] ? ASBString(@{@"case":NSUUID.UUID.UUIDString, @"page":page, @"provider":test[@"provider"]}) : [NSUUID.UUID.UUIDString stringByAppendingFormat:@":%@", page];
                    paired(endpoints, ^id(ASBEndpoint *endpoint) { [endpoint reset:epoch]; return nil; });
                }
                NSMutableDictionary *expected = [@{@"count":@0, @"checked":@NO, @"range":@4, @"text":@"initial", @"selection":@[@0,@0], @"case":epoch, @"page":page} mutableCopy];
                [expected addEntriesFromDictionary:test[@"initialState"] ?: @{}];
                NSDictionary *warmup = settled(endpoints, expected, timeout, nil); item[@"reset"] = @(reset); item[@"resetSeconds"] = @(NSProcessInfo.processInfo.systemUptime - caseStart); item[@"resetObservations"] = warmup[@"observations"];
                ASBRequire([warmup[@"errors"] count] == 0, [@"Reset state did not converge: " stringByAppendingString:ASBString(warmup[@"errors"])]);
                if (reset) paired(endpoints, ^id(ASBEndpoint *endpoint) { [endpoint clear]; return nil; });
                for (NSDictionary *step in test[@"setup"]) {
                    NSArray *replies = paired(endpoints, ^id(ASBEndpoint *endpoint) { return [endpoint call:step[@"target"] operations:step[@"operations"] extra:nil]; });
                    ASBRequire(!hasError(replies), [@"Setup operation failed: " stringByAppendingString:ASBString(replies)]);
                    if (step[@"expectedState"]) { NSMutableDictionary *state = expected.mutableCopy; [state addEntriesFromDictionary:step[@"expectedState"]]; ASBRequire([settled(endpoints, state, timeout, nil)[@"errors"] count] == 0, @"Setup state did not converge"); }
                }
                NSArray *operations = paired(endpoints, ^id(ASBEndpoint *endpoint) { return [endpoint call:test[@"target"] operations:test[@"operations"] extra:test[@"observe"] ? @{@"observe":test[@"observe"]} : nil]; });
                item[@"operations"] = operations; item[@"comparison"] = test[@"comparison"] ?: @"typed-values-with-geometry-color-roundoff";
                NSMutableArray *diffs = [parity(operations,test,@"/operations") mutableCopy];
                NSMutableArray *baselineDiffs = NSMutableArray.array;
                NSMutableArray *nativeReplies = [NSMutableArray arrayWithObject:operations[0]];
                BOOL invalidRead = interruptedOperation(operations);
                if (test[@"expectedResults"]) { [baselineDiffs addObjectsFromArray:ASBResultDifferences(test[@"expectedResults"], ASBCompared(test, operations[0][@"results"]), @"/expected-native")]; if (!nativeOnly) [diffs addObjectsFromArray:ASBResultDifferences(test[@"expectedResults"], ASBCompared(test, operations[1][@"results"]), @"/expected-proxy")]; }
                if (test[@"observe"]) {
                    if (!nativeOnly) [diffs addObjectsFromArray:ASBDifferences(operations[0][@"registrations"], operations[1][@"registrations"], @"/registrations")];
                    for (NSUInteger side = 0; side < operations.count; side++) { NSMutableSet *seen = NSMutableSet.set; for (NSDictionary *event in operations[side][@"events"]) [seen addObject:event[@"name"]];
                        for (NSString *name in test[@"observe"][@"names"]) if (![seen containsObject:name]) [(side == 0 ? baselineDiffs : diffs) addObject:@{@"side":@(side), @"path":@"/events", @"expected":name, @"actual":seen.allObjects}]; }
                }
                [expected addEntriesFromDictionary:test[@"expectedState"]];
                if (!hasError(operations)) {
                    NSArray *revisions = [test[@"mutation"] boolValue] ? [warmup[@"states"] valueForKey:@"revision"] : nil;
                    NSDictionary *settle = settled(endpoints, expected, timeout, revisions); item[@"observations"] = settle[@"observations"]; [(nativeOnly ? baselineDiffs : diffs) addObjectsFromArray:settle[@"errors"]];
                }
                if ([test[@"reads"] count]) { NSMutableArray *reads = NSMutableArray.array; for (NSString *field in test[@"reads"]) [reads addObject:@{@"kind":@"get", @"name":field}];
                    NSArray *replies = paired(endpoints, ^id(ASBEndpoint *endpoint) { return [endpoint call:test[@"target"] operations:reads extra:nil]; }); item[@"reads"] = replies; [nativeReplies addObject:replies[0]]; invalidRead |= interruptedOperation(replies); [diffs addObjectsFromArray:parity(replies,@{},@"/reads")]; }
                NSMutableArray *after = NSMutableArray.array;
                for (NSDictionary *step in test[@"after"]) {
                    NSArray *replies = paired(endpoints, ^id(ASBEndpoint *endpoint) { return [endpoint call:step[@"target"] operations:step[@"operations"] extra:nil]; });
                    NSString *path = [NSString stringWithFormat:@"/after/%lu",(unsigned long)after.count];
                    [after addObject:replies]; [nativeReplies addObject:replies[0]]; invalidRead |= interruptedOperation(replies);
                    [diffs addObjectsFromArray:parity(replies,step,path)];
                    if (step[@"expectedResults"]) {
                        [baselineDiffs addObjectsFromArray:ASBResultDifferences(step[@"expectedResults"],ASBCompared(step,replies[0][@"results"]),[path stringByAppendingString:@"/expected-native"])];
                        if (!nativeOnly) [diffs addObjectsFromArray:ASBResultDifferences(step[@"expectedResults"],ASBCompared(step,replies[1][@"results"]),[path stringByAppendingString:@"/expected-proxy"])];
                    }
                }
                item[@"after"] = after; item[@"differences"] = diffs; item[@"baselineDifferences"] = baselineDiffs;
                NSString *name = test[@"id"]; BOOL negative = [name hasPrefix:@"negative."] || [name containsString:@".invalid"] || [name containsString:@".past-end"] || [name hasSuffix:@"novalue"] || [name containsString:@"multiple"];
                item[@"status"] = baselineDiffs.count ? @"BASELINE_MISMATCH" : diffs.count ? @"FAIL" : hasError(nativeReplies) && !negative ? nativeErrorStatus(nativeReplies) : successful;
                if (invalidRead) item[@"status"] = @"INCOMPLETE";
                if (!frontmost) frontmost = [left.trace firstObject][@"frontmostBefore"];
                BOOL desktopChanged = NO; for (NSDictionary *trace in [left.trace arrayByAddingObjectsFromArray:right.trace]) for (NSString *key in @[@"frontmostBefore", @"frontmostAfter"]) if (!ASBEqual(trace[key], frontmost)) desktopChanged = YES;
                item[@"desktopChanged"] = @(desktopChanged); if (desktopChanged && [test[@"desktop"] isEqual:@"focus"]) item[@"status"] = @"INTERRUPTED";
                previousMutation = [test[@"mutation"] boolValue]; previousPage = page; previousProvider = test[@"provider"];
            } @catch (NSException *failure) { item[@"reason"] = failure.reason ?: failure.name; epoch = nil; }
            for (NSDictionary *trace in [left.trace arrayByAddingObjectsFromArray:right.trace])
                if ([trace[@"sessionAvailableBefore"] isEqual:@NO] || [trace[@"sessionAvailableAfter"] isEqual:@NO]) { item[@"status"] = @"INTERRUPTED"; stop = YES; }
            item[@"seconds"] = @(NSProcessInfo.processInfo.systemUptime - caseStart); [results addObject:item];
            ASBSave([output stringByAppendingPathComponent:[NSString stringWithFormat:@"%02ld-%@.json", (long)repetition, test[@"id"]]], @{@"result":item, @"nativeTrace":left.trace, @"proxyTrace":right.trace});
            printf("%s %s %.3fs\n", [item[@"status"] UTF8String], [test[@"id"] UTF8String], [item[@"seconds"] doubleValue]); fflush(stdout);
            double now = NSProcessInfo.processInfo.systemUptime;
            if (now - checkpointAt >= 1) {
                ASBSave([output stringByAppendingPathComponent:@"result.json"], @{@"schema":@"asb-ax-conformance-run-1", @"cases":results, @"elapsedSeconds":@(now - started)});
                checkpointAt = now;
            }
            if (stop || ([options[@"fail-fast"] boolValue] && ![item[@"status"] isEqual:successful])) { stop = YES; break; }
        }}
    } @catch (NSException *failure) {
        ASBSave([output stringByAppendingPathComponent:@"result.json"],@{@"schema":@"asb-ax-conformance-run-1",@"status":@"INCOMPLETE",@"reason":failure.reason ?: failure.name,@"cases":results});
        ASBSave([output stringByAppendingPathComponent:@"incomplete-traces.json"],@{@"nativeTrace":left.trace,@"proxyTrace":right.trace});
        @throw failure;
    } @finally { [left.probe close]; [right.probe close]; }
    NSMutableArray *times = NSMutableArray.array; NSMutableDictionary *counts = NSMutableDictionary.dictionary;
    for (NSDictionary *item in results) { counts[item[@"status"]] = @([counts[item[@"status"]] integerValue] + 1); if (item[@"seconds"]) [times addObject:item[@"seconds"]]; }
    [times sortUsingSelector:@selector(compare:)]; id median = times.count ? @(([times[(times.count - 1) / 2] doubleValue] + [times[times.count / 2] doubleValue]) / 2) : NSNull.null;
    double elapsed = NSProcessInfo.processInfo.systemUptime - started;
    ASBSave([output stringByAppendingPathComponent:@"result.json"], @{@"schema":@"asb-ax-conformance-run-1", @"cases":results, @"elapsedSeconds":@(elapsed), @"readerCalls":@(left.probe.calls + right.probe.calls), @"medianCaseSeconds":median, @"p95CaseSeconds":times.count ? times[MIN(times.count - 1, (NSUInteger)(.95 * times.count))] : NSNull.null});
    puts(ASBString(@{@"results":counts, @"seconds":@(elapsed), @"medianCaseSeconds":median, @"output":output}).UTF8String);
    return [counts[successful] unsignedIntegerValue] == results.count ? 0 : 1;
}
