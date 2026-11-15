#import "suite-common.h"
#include <signal.h>
#include <errno.h>

int ASBSelfTest(void) {
    ASBRequire(!ASBEqual(@YES,@1) && ASBEqual(@1,@1.0),@"Scalar comparison changed");
    ASBRequire(ASBDifferences(@[@{@"value":@YES}],@[@{@"value":@1}],@"").count == 1,@"Nested boolean loss went undetected");
    id (^point)(id) = ^id(id x) { return @{@"type":@"point",@"value":@[x,@34]}; };
    ASBRequire(!ASBResultDifferences(point(@12),point(@11.999999999999998),@"").count,@"Coordinate round-trip noise rejected");
    ASBRequire(ASBResultDifferences(point(@12),point(@12.000001),@"").count &&
        ASBResultDifferences(point(@1),point(@YES),@"").count,@"Coordinate change or boolean loss masked");
    ASBRequire(ASBResultDifferences(@{@"type":@"double",@"value":@12},@{@"type":@"double",@"value":@11.999999999999998},@"").count,@"Scalar precision loss masked");
    id (^color)(double) = ^id(double r) { return @{@"type":@"color",@"value":@[@(r),@0,@0,@1]}; };
    ASBRequire(!ASBResultDifferences(@[color(.066666670143604279)],@[color(.066666670143604251)],@"").count &&
        ASBResultDifferences(color(.1),color(.10001),@"").count,@"Color comparison precision changed");
    NSDictionary *a = @{@"type":@"element",@"ref":@17,@"identity":@{@"AXRole":@"AXButton",@"AXTitle":@"Button"}};
    NSDictionary *b = @{@"type":@"element",@"ref":@42,@"identity":a[@"identity"]};
    ASBRequire(ASBEqual(ASBStable(@[a,a]),ASBStable(@[b,b])) && !ASBEqual(ASBStable(@[a,a]),ASBStable(@[a,b])),@"Element identity or aliases changed");
    NSMutableDictionary *differentTitle = [b mutableCopy]; differentTitle[@"identity"] = @{@"AXRole":@"AXButton",@"AXTitle":@"Changed"};
    ASBRequire(!ASBEqual(ASBStable(@[a]),ASBStable(@[differentTitle])),@"Element title loss masked");
    NSArray *(^rtf)(NSString *) = ^NSArray *(NSString *text) { return @[@{@"error":@0,@"value":@{@"type":@"data",@"value":[[text dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]}}]; };
    NSDictionary *mode = @{@"comparison":@"rtf-except-generator-version"};
    ASBRequire(ASBEqual(ASBCompared(mode,rtf(@"{\\rtf1\\cocoartf2908 text}")),ASBCompared(mode,rtf(@"{\\rtf1\\cocoartf2870 text}"))),@"RTF generator version not normalized");
    ASBRequire(!ASBEqual(ASBCompared(mode,rtf(@"{\\rtf1\\cocoartf2908 text}")),ASBCompared(mode,rtf(@"{\\rtf1\\cocoartf2870 changed}"))),@"RTF content change masked");
    NSDictionary *typed = @{@"error":@0,@"value":@{@"type":@"integer",@"value":@1}}, *boolean = @{@"error":@0,@"value":@{@"type":@"boolean",@"value":@YES}};
    ASBRequire(!ASBEqual(ASBCompared(@{},@[typed]),ASBCompared(@{},@[boolean])),@"Typed result distinction lost");
    ASBRequire([ASBProcess(@[@"/usr/bin/printf",@"%s",@"literal $(not-a-command) `nothing`"],nil,5,YES)[@"stdout"] isEqual:@"literal $(not-a-command) `nothing`"],@"Process arguments changed");
    BOOL timeout = NO; @try { ASBProcess(@[@"/bin/sleep",@"2"],nil,.02,YES); } @catch (NSException *error) { timeout = [error.reason containsString:@"exceeded"]; } ASBRequire(timeout,@"Subprocess deadline not enforced");
    ASBTreeSelfTest(); NSArray *cases = ASBCases();
    printf("PASS: comparison guards, subtree integrity, baseline changes, process deadlines; %lu case definitions loaded.\n",(unsigned long)cases.count); return 0;
}
static int stopHost(NSDictionary *options) {
    ASBAPI *api = ASBAPI.new; NSArray *names = [ASBRequired(options,@"vms") componentsSeparatedByString:@","]; NSMutableArray *paths = NSMutableArray.array;
    for (NSString *name in names) { NSString *path = [api vmPath:name]; [paths addObject:path]; if ([[api request:@"GET" path:path][@"running"] boolValue]) [api request:@"POST" path:[path stringByAppendingString:@"/shutdown"]]; }
    double deadline = NSProcessInfo.processInfo.systemUptime + 120;
    for (;;) { BOOL running = NO; for (NSString *path in paths) if ([[api request:@"GET" path:path][@"running"] boolValue]) running = YES; if (!running) break;
        ASBRequire(NSProcessInfo.processInfo.systemUptime < deadline,@"Guest shutdown did not complete; existing host left running"); [NSThread sleepForTimeInterval:.5]; }
    [api request:@"POST" path:@"/shutdown"];
    deadline = NSProcessInfo.processInfo.systemUptime + 30;
    pid_t hostPID = [api.discovery[@"pid"] intValue];
    while (kill(hostPID,0) == 0 || errno == EPERM) { ASBRequire(NSProcessInfo.processInfo.systemUptime < deadline,@"Host shutdown did not complete"); [NSThread sleepForTimeInterval:.1]; }
    return 0;
}
static int startViewer(NSDictionary *options) {
    double deadline = NSProcessInfo.processInfo.systemUptime + 60; ASBAPI *api = nil;
    for (;;) { @try { api = ASBAPI.new; [api request:@"GET" path:@"/version"]; break; } @catch (NSException *error) { if (NSProcessInfo.processInfo.systemUptime >= deadline) @throw error; [NSThread sleepForTimeInterval:.5]; } }
    NSString *path = [api vmPath:ASBRequired(options,@"vm")];
    if (![[api request:@"GET" path:path][@"running"] boolValue]) [api request:@"POST" path:[path stringByAppendingString:@"/start"]];
    deadline = NSProcessInfo.processInfo.systemUptime + 120;
    while (![[api request:@"GET" path:[path stringByAppendingString:@"/display"]][@"ready"] boolValue]) { ASBRequire(NSProcessInfo.processInfo.systemUptime < deadline,@"VM is still starting; display readiness deadline"); [NSThread sleepForTimeInterval:.5]; }
    [api request:@"POST" path:[path stringByAppendingString:@"/display"]]; puts("Combined native tester and VM viewer opened. Terminal can be closed."); return 0;
}
int main(int argc, const char *argv[]) {
    @autoreleasepool { @try {
        signal(SIGPIPE,SIG_IGN); setbuf(stdout,NULL);
        ASBRequire(argc >= 2,@"Usage: ax-suite {list|run|stress|catalog|coverage|build-host|verify-host|prepare|tree-snapshot|tree-pair|tree-compare|self-test|api|stop-host|start-viewer} [options]");
        NSString *executable = [NSString stringWithUTF8String:argv[0]]; if (!executable.isAbsolutePath) executable = [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:executable];
        ASBDirectory = executable.stringByResolvingSymlinksInPath.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
        NSMutableArray *arguments = NSMutableArray.array; for (int i = 2; i < argc; i++) [arguments addObject:@(argv[i])];
        NSDictionary *options = ASBOptions(arguments); if (options[@"directory"]) ASBDirectory = options[@"directory"];
        NSString *command = @(argv[1]);
        NSDictionary *allowed = @{@"list":@"",@"self-test":@"",@"fixture-revision":@"",@"run":@"vm paired-pid native-pid window proxy-under cases repeat timeout output failed-from fail-fast include-focus native-only",@"stress":@"vm paired-pid native-pid window proxy-under cases duration output",@"catalog":@"sdk output check",@"coverage":@"run output",@"build-host":@"output verify-only",@"verify-host":@"app",@"prepare":@"vm build local",@"api":@"method path",@"stop-host":@"vms",@"start-viewer":@"vm",@"tree-snapshot":@"pid window attributes output timeout max-nodes",@"tree-compare":@"spec baseline candidate baseline-after output",@"tree-pair":@"spec baseline-pid candidate-pid vm vm-timeout baseline-window candidate-window output-dir timeout max-nodes"};
        ASBRequire(allowed[command] != nil,[@"Unknown command: " stringByAppendingString:command]);
        NSArray *keys = [[allowed[command] stringByAppendingString:@" directory"] componentsSeparatedByString:@" "];
        for (NSString *key in options) ASBRequire([keys containsObject:key],[@"Unknown option: --" stringByAppendingString:key]);
        if ([command isEqual:@"list"]) { puts(ASBString(ASBCases()).UTF8String); return 0; }
        if ([command isEqual:@"run"]) return ASBRun(options);
        if ([command isEqual:@"stress"]) return ASBStressRun(options);
        if ([command isEqual:@"catalog"]) return ASBCatalog(options);
        if ([command isEqual:@"coverage"]) return ASBCoverage(options);
        if ([command isEqual:@"build-host"]) return ASBBuildHost(options);
        if ([command isEqual:@"verify-host"]) return ASBVerifyHost(ASBRequired(options,@"app"));
        if ([command isEqual:@"prepare"]) return ASBPrepare(options);
        if ([@[@"tree-snapshot",@"tree-pair",@"tree-compare"] containsObject:command]) return ASBTreeCommand(command,options);
        if ([command isEqual:@"self-test"]) return ASBSelfTest();
        if ([command isEqual:@"fixture-revision"]) { puts(ASBFixtureRevision().UTF8String); return 0; }
        if ([command isEqual:@"api"]) { puts(ASBString([ASBAPI.new request:options[@"method"] ?: @"GET" path:ASBRequired(options,@"path")]).UTF8String); return 0; }
        if ([command isEqual:@"stop-host"]) return stopHost(options);
        if ([command isEqual:@"start-viewer"]) return startViewer(options);
        ASBRequire(NO,[@"Unknown command: " stringByAppendingString:command]);
    } @catch (NSException *error) { fprintf(stderr,"%s\n",(error.reason ?: error.name).UTF8String); return 2; } }
    return 0;
}
