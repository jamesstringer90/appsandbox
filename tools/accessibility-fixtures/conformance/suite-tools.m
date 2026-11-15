#import "suite-common.h"
#include <unistd.h>

static NSDictionary *signature(NSString *path) {
    NSString *output = ASBProcess(@[@"/usr/bin/codesign",@"-dvv",path], nil, 20, YES)[@"stderr"]; NSMutableDictionary *fields = NSMutableDictionary.dictionary;
    for (NSString *line in [output componentsSeparatedByString:@"\n"]) { NSRange split = [line rangeOfString:@"="]; if (split.location != NSNotFound) { NSString *key = [line substringToIndex:split.location]; if (!fields[key]) fields[key] = [line substringFromIndex:split.location + 1]; } }
    ASBRequire([fields[@"Authority"] length] && [fields[@"TeamIdentifier"] length] && ![fields[@"TeamIdentifier"] isEqual:@"not set"], @"Test app must retain its Xcode signing identity"); return fields;
}
int ASBVerifyHost(NSString *app) {
    ASBProcess(@[@"/usr/bin/codesign",@"--verify",@"--deep",@"--strict",app], nil, 30, YES);
    ASBRequire([signature(app)[@"TeamIdentifier"] isEqual:signature([app stringByAppendingPathComponent:@"Contents/Frameworks/AppSandboxCore.framework"])[@"TeamIdentifier"]], @"App and AppSandboxCore framework have different signing teams");
    ASBRequire(geteuid() != 0, @"Run this check without sudo; the launch check must remain unprivileged");
    NSDictionary *result = ASBProcess(@[[app stringByAppendingPathComponent:@"Contents/MacOS/AppSandbox"],@"--headless"], nil, 15, NO);
    ASBRequire([result[@"status"] intValue] == 3 && [result[@"stderr"] containsString:@"AppSandbox --headless must be run elevated"], @"App failed the unprivileged launch check");
    puts("Signature and unprivileged launch checks passed."); return 0;
}
static NSString *absolute(NSString *path) { return (path.isAbsolutePath ? path : [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path]).stringByStandardizingPath; }
static void replace(NSMutableString *source, NSString *needle, NSString *value) {
    ASBRequire([source componentsSeparatedByString:needle].count == 2, [@"Project insertion must match once: " stringByAppendingString:needle]);
    [source replaceOccurrencesOfString:needle withString:value options:0 range:NSMakeRange(0, source.length)];
}
int ASBBuildHost(NSDictionary *options) {
    if (options[@"verify-only"]) return ASBVerifyHost(absolute(options[@"verify-only"]));
    NSString *root = ASBDirectory.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    NSString *output = absolute(options[@"output"] ?: [ASBDirectory stringByAppendingPathComponent:@"build/host"]);
    NSString *app = [output stringByAppendingPathComponent:@"Products/Release/AppSandbox.app"];
    NSDictionary *running = ASBProcess(@[@"/bin/ps",@"-axo",@"command="], nil, 10, YES);
    ASBRequire(![running[@"stdout"] containsString:[app stringByAppendingPathComponent:@"Contents/MacOS/AppSandbox --headless"]], @"Cannot rebuild the running host; choose a different output directory");
    NSFileManager *fm = NSFileManager.defaultManager; NSString *project = [output stringByAppendingPathComponent:@"AppSandbox.xcodeproj"];
    [fm createDirectoryAtPath:project withIntermediateDirectories:YES attributes:nil error:NULL];
    NSMutableString *source = [[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"AppSandbox.xcodeproj/project.pbxproj"] encoding:NSUTF8StringEncoding error:NULL] mutableCopy];
    ASBRequire(source != nil, @"Cannot read production Xcode project"); NSMutableString *additions = NSMutableString.string; NSMutableArray *buildIDs = NSMutableArray.array, *frameworkIDs = NSMutableArray.array;
    NSArray *filenames = @[@"paired-window.m",@"fixture-view.m",@"fixture-pages.m",@"fixture-advanced.m",@"fixture-stress.m"];
    for (NSUInteger i = 0; i < filenames.count; i++) {
        NSString *ref = [NSString stringWithFormat:@"ACCE57E57000000000000%03lX",(unsigned long)i], *build = [NSString stringWithFormat:@"ACCE57E57100000000000%03lX",(unsigned long)i];
        [additions appendFormat:@"%@ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; path = %@; sourceTree = \"<absolute>\"; };\n%@ = {isa = PBXBuildFile; fileRef = %@; settings = {COMPILER_FLAGS = \"-Wno-deprecated-declarations\"; }; };\n", ref, ASBString([ASBDirectory stringByAppendingPathComponent:filenames[i]]), build, ref]; [buildIDs addObject:[build stringByAppendingString:@","]];
    }
    NSArray *frameworks = @[@"Accessibility",@"WebKit"];
    for (NSUInteger i = 0; i < frameworks.count; i++) {
        NSString *ref = [NSString stringWithFormat:@"ACCE57E5700000000000001%lu",(unsigned long)i], *build = [NSString stringWithFormat:@"ACCE57E5710000000000001%lu",(unsigned long)i], *name = frameworks[i];
        [additions appendFormat:@"%@ = {isa = PBXFileReference; lastKnownFileType = wrapper.framework; name = %@.framework; path = System/Library/Frameworks/%@.framework; sourceTree = SDKROOT; };\n%@ = {isa = PBXBuildFile; fileRef = %@; };\n",ref,name,name,build,ref]; [frameworkIDs addObject:[build stringByAppendingString:@","]];
    }
    replace(source, @"objects = {", [@"objects = {\n" stringByAppendingString:additions]);
    for (NSArray *pair in @[@[@"2001000000000000000000AA /* main.m in Sources */,",buildIDs], @[@"4001000000000000000000AA /* Cocoa.framework in Frameworks */,",frameworkIDs]]) replace(source, pair[0], [pair[0] stringByAppendingFormat:@"\n%@", [pair[1] componentsJoinedByString:@"\n"]]);
    replace(source, @"projectDirPath = \"\";", [NSString stringWithFormat:@"projectDirPath = %@;",ASBString(root)]);
    [source writeToFile:[project stringByAppendingPathComponent:@"project.pbxproj"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    NSTask *task = NSTask.new; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcodebuild"];
    task.arguments = @[@"-project",project,@"-target",@"AppSandbox",@"-configuration",@"Release",[@"SRCROOT=" stringByAppendingString:root],[@"PROJECT_DIR=" stringByAppendingString:root],[@"SYMROOT=" stringByAppendingString:[output stringByAppendingPathComponent:@"Products"]],[@"OBJROOT=" stringByAppendingString:[output stringByAppendingPathComponent:@"Intermediates"]],@"build"];
    NSError *error = nil; ASBRequire([task launchAndReturnError:&error],error.localizedDescription); [task waitUntilExit]; ASBRequire(task.terminationStatus == 0,@"Xcode build failed");
    NSString *identity = signature(app)[@"Authority"], *infoPath = [app stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSString *revision = ASBFixtureRevision();
    ASBRequire([ASBLoad([ASBDirectory stringByAppendingPathComponent:@"build/fixture-revision.json"])[@"sha256"] isEqual:revision],@"Rebuild the fixture before building the paired host");
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithContentsOfFile:infoPath]; info[@"ASBAXConformanceSourceDirectory"] = ASBDirectory; info[@"ASBAXFixtureRevision"] = revision; ASBRequire([info writeToFile:infoPath atomically:YES],@"Cannot update test app metadata");
    NSString *resources = [app stringByAppendingPathComponent:@"Contents/Resources/AXConformance"];
    if ([fm fileExistsAtPath:resources]) ASBRequire([fm removeItemAtPath:resources error:&error],error.localizedDescription);
    ASBRequire([fm createDirectoryAtPath:[resources stringByAppendingPathComponent:@"build"] withIntermediateDirectories:YES attributes:nil error:&error],error.localizedDescription);
    for (NSString *file in @[@"cases.json",@"stress-cases.json",@"macos-sdk.json",@"web-fixture.html",@"build/ax-probe",@"build/ax-suite",@"build/fixture-revision.json"]) ASBRequire([fm copyItemAtPath:[ASBDirectory stringByAppendingPathComponent:file] toPath:[resources stringByAppendingPathComponent:file] error:&error],error.localizedDescription);
    ASBProcess(@[@"/usr/bin/codesign",@"--force",@"--sign",identity,@"--preserve-metadata=identifier,entitlements,flags",app],nil,60,YES);
    ASBVerifyHost(app); puts(app.UTF8String); return 0;
}
int ASBPrepare(NSDictionary *options) {
    if ([options[@"build"] boolValue]) ASBProcess(@[@"/bin/sh",[ASBDirectory stringByAppendingPathComponent:@"build.sh"]],nil,120,YES);
    ASBAPI *api = ASBAPI.new; NSString *vmPath = [api vmPath:ASBRequired(options,@"vm")]; NSDictionary *vm = [api request:@"GET" path:vmPath];
    ASBRequire([vm[@"running"] boolValue] && [vm[@"osType"] isEqual:@"macOS"],@"Preparation requires a running macOS guest");
    NSDictionary *sshInfo = [api request:@"GET" path:[vmPath stringByAppendingString:@"/sshInfo"]]; NSString *key = [api.support stringByAppendingPathComponent:@"ssh/id_appsandbox"], *user = sshInfo[@"user"], *port = [sshInfo[@"port"] description];
    NSString *guest = [NSString stringWithFormat:@"/Users/%@/Documents/AppSandbox-QA/ax-conformance",user], *target = [user stringByAppendingString:@"@127.0.0.1"];
    NSArray *ssh = @[@"/usr/bin/ssh",@"-i",key,@"-o",@"BatchMode=yes",@"-o",@"StrictHostKeyChecking=accept-new",@"-p",port,target], *scp = @[@"/usr/bin/scp",@"-i",key,@"-o",@"BatchMode=yes",@"-o",@"StrictHostKeyChecking=accept-new",@"-P",port];
    NSString *app = [ASBDirectory stringByAppendingPathComponent:@"build/ASBAXConformance.app"], *probe = [ASBDirectory stringByAppendingPathComponent:@"build/ax-probe"];
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableString *manifest = NSMutableString.string; NSMutableDictionary *hashes = NSMutableDictionary.dictionary;
    for (NSString *relative in [[[fm enumeratorAtPath:app] allObjects] sortedArrayUsingSelector:@selector(compare:)]) { NSString *path = [app stringByAppendingPathComponent:relative]; BOOL directory = NO; [fm fileExistsAtPath:path isDirectory:&directory]; if (!directory) { hashes[relative] = ASBHash(path); [manifest appendFormat:@"%@  ASBAXConformance.app/%@\n",hashes[relative],relative]; } }
    ASBRequire(hashes.count > 0,@"Build the fixture first"); NSString *expected = ASBHashValue(hashes);
    NSString *check = [NSString stringWithFormat:@"cd %@ && /usr/bin/shasum -a 256 -c -",ASBQuote(guest)];
    BOOL changed = [ASBProcess([ssh arrayByAddingObject:check],[manifest dataUsingEncoding:NSUTF8StringEncoding],30,NO)[@"status"] intValue] != 0;
    ASBProcess([ssh arrayByAddingObject:[@"mkdir -p " stringByAppendingString:ASBQuote(guest)]],nil,20,YES);
    if (changed) {
        NSString *temporary = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]; [fm createDirectoryAtPath:temporary withIntermediateDirectories:YES attributes:nil error:NULL];
        @try {
            NSString *archive = [temporary stringByAppendingPathComponent:@"fixture.zip"];
            ASBProcess(@[@"/usr/bin/ditto",@"-c",@"-k",@"--sequesterRsrc",@"--keepParent",app,archive],nil,30,YES);
            ASBProcess([scp arrayByAddingObjectsFromArray:@[archive,[target stringByAppendingFormat:@":%@/",guest]]],nil,60,YES);
            NSString *command = [NSString stringWithFormat:@"for p in $(pgrep -x ASBAXConformance); do case \"$(ps -p \"$p\" -o command=)\" in %@/*) kill \"$p\" ;; esac; done; cd %@ && ditto -x -k fixture.zip . && open -n ASBAXConformance.app --args --guest",ASBQuote(guest),ASBQuote(guest)];
            ASBProcess([ssh arrayByAddingObject:command],nil,30,YES);
        } @finally { [fm removeItemAtPath:temporary error:NULL]; }
    } else ASBProcess([ssh arrayByAddingObject:[@"open " stringByAppendingString:ASBQuote([guest stringByAppendingPathComponent:@"ASBAXConformance.app"])]],nil,20,YES);
    ASBProcess([ssh arrayByAddingObject:check],[manifest dataUsingEncoding:NSUTF8StringEncoding],30,YES);
    NSString *probeHash = ASBHash(probe); NSDictionary *inspect = ASBProcess([ssh arrayByAddingObject:[@"shasum -a 256 " stringByAppendingString:ASBQuote([guest stringByAppendingPathComponent:@"ax-probe"])]],nil,20,NO);
    BOOL probeChanged = [inspect[@"status"] intValue] != 0 || ![inspect[@"stdout"] hasPrefix:probeHash];
    if (probeChanged) { ASBProcess([scp arrayByAddingObjectsFromArray:@[probe,[target stringByAppendingFormat:@":%@/",guest]]],nil,30,YES); ASBProcess([ssh arrayByAddingObject:[@"chmod +x " stringByAppendingString:ASBQuote([guest stringByAppendingPathComponent:@"ax-probe"])]],nil,20,YES); }
    if ([options[@"local"] boolValue]) ASBProcess(@[@"/usr/bin/open",@"-n",app],nil,20,YES);
    puts(ASBString(@{@"vm":vm[@"name"],@"fixtureUpdated":@(changed),@"fixtureBundleSHA256":expected,@"probeUpdated":@(probeChanged),@"probeSHA256":probeHash,@"guestDirectory":guest}).UTF8String); return 0;
}
