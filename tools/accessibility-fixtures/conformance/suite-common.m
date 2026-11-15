#import "suite-common.h"
#import <CommonCrypto/CommonDigest.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <float.h>
#include <math.h>

NSString *ASBDirectory;
void ASBRequire(BOOL condition, NSString *message) {
    if (!condition) @throw [NSException exceptionWithName:@"ASBTestError" reason:message userInfo:nil];
}
id ASBParse(NSData *data) {
    NSError *error = nil;
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers | NSJSONReadingFragmentsAllowed error:&error] : nil;
    ASBRequire(value != nil, error.localizedDescription ?: @"Missing JSON data"); return value;
}
id ASBLoad(NSString *path) { return ASBParse([NSData dataWithContentsOfFile:path]); }
NSData *ASBJSON(id value) {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys | NSJSONWritingFragmentsAllowed | NSJSONWritingWithoutEscapingSlashes error:&error];
    ASBRequire(data != nil, error.localizedDescription); return data;
}
NSString *ASBString(id value) { return [[NSString alloc] initWithData:ASBJSON(value) encoding:NSUTF8StringEncoding]; }
void ASBSave(NSString *path, id value) {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted error:&error];
    ASBRequire(data && [data writeToFile:path options:NSDataWritingAtomic error:&error], error.localizedDescription ?: @"Cannot save JSON");
}
static NSString *hashData(NSData *data) {
    ASBRequire(data != nil, @"Cannot hash missing data");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *result = NSMutableString.string;
    for (unsigned i = 0; i < sizeof(digest); i++) [result appendFormat:@"%02x", digest[i]];
    return result;
}
NSString *ASBHash(NSString *path) { return hashData([NSData dataWithContentsOfFile:path]); }
NSString *ASBHashValue(id value) { return hashData(ASBJSON(value)); }
NSString *ASBFixtureRevision(void) {
    NSMutableDictionary *hashes = NSMutableDictionary.dictionary;
    for (NSString *file in @[@"fixture-view.m",@"fixture-view.h",@"fixture-pages.m",@"fixture-pages.h",@"fixture-advanced.m",@"fixture-advanced.h",@"fixture-stress.m",@"fixture-stress.h",@"macos-sdk.json",@"web-fixture.html"])
        hashes[file] = ASBHash([ASBDirectory stringByAppendingPathComponent:file]);
    return ASBHashValue(hashes);
}
NSString *ASBQuote(NSString *value) { return [NSString stringWithFormat:@"'%@'", [value stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]]; }
NSDictionary *ASBProcess(NSArray<NSString *> *arguments, NSData *input, double timeout, BOOL check) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [fm createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    NSTask *task = NSTask.new;
    NSString *out = [directory stringByAppendingPathComponent:@"out"], *err = [directory stringByAppendingPathComponent:@"err"], *in = [directory stringByAppendingPathComponent:@"in"];
    [fm createFileAtPath:out contents:nil attributes:nil]; [fm createFileAtPath:err contents:nil attributes:nil];
    [(input ?: NSData.data) writeToFile:in atomically:YES];
    NSFileHandle *stdoutHandle = [NSFileHandle fileHandleForWritingAtPath:out], *stderrHandle = [NSFileHandle fileHandleForWritingAtPath:err], *stdinHandle = [NSFileHandle fileHandleForReadingAtPath:in];
    @try {
        task.executableURL = [NSURL fileURLWithPath:arguments[0]]; task.arguments = [arguments subarrayWithRange:NSMakeRange(1, arguments.count - 1)];
        task.standardOutput = stdoutHandle; task.standardError = stderrHandle; task.standardInput = stdinHandle;
        NSError *error = nil; ASBRequire([task launchAndReturnError:&error], error.localizedDescription);
        double deadline = NSProcessInfo.processInfo.systemUptime + timeout;
        while (task.running && NSProcessInfo.processInfo.systemUptime < deadline) [NSThread sleepForTimeInterval:.01];
        BOOL timedOut = task.running;
        if (timedOut) kill(task.processIdentifier, SIGKILL);
        [task waitUntilExit];
        ASBRequire(!timedOut, [NSString stringWithFormat:@"%@ exceeded %.1fs", arguments[0], timeout]);
        NSString *output = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:NULL] ?: @"";
        NSString *errors = [NSString stringWithContentsOfFile:err encoding:NSUTF8StringEncoding error:NULL] ?: @"";
        ASBRequire(!check || task.terminationStatus == 0, [NSString stringWithFormat:@"%@ exited %d: %@", arguments[0], task.terminationStatus, errors]);
        return @{@"status":@(task.terminationStatus), @"stdout":output, @"stderr":errors};
    } @finally {
        if (task.running) { kill(task.processIdentifier, SIGKILL); [task waitUntilExit]; }
        [stdoutHandle closeFile]; [stderrHandle closeFile]; [stdinHandle closeFile]; [fm removeItemAtPath:directory error:NULL];
    }
}
NSDictionary *ASBOptions(NSArray<NSString *> *arguments) {
    NSMutableDictionary *result = NSMutableDictionary.dictionary;
    NSSet *flags = [NSSet setWithArray:@[@"check", @"fail-fast", @"include-focus", @"native-only", @"build", @"local"]];
    for (NSUInteger i = 0; i < arguments.count; i++) {
        NSString *name = arguments[i]; ASBRequire([name hasPrefix:@"--"], [@"Expected option: " stringByAppendingString:name]); name = [name substringFromIndex:2];
        if ([flags containsObject:name]) result[name] = @YES;
        else { ASBRequire(i + 1 < arguments.count, [@"Missing value: " stringByAppendingString:name]);
            if ([name isEqual:@"run"]) { if (!result[name]) result[name] = NSMutableArray.array; [result[name] addObject:arguments[++i]]; }
            else { ASBRequire(!result[name], [@"Duplicate option: " stringByAppendingString:name]); result[name] = arguments[++i]; }
        }
    }
    return result;
}
NSString *ASBRequired(NSDictionary *options, NSString *name) { ASBRequire([options[name] isKindOfClass:NSString.class] && [options[name] length], [@"Required: --" stringByAppendingString:name]); return options[name]; }
static BOOL boolean(id value) { return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID(); }
BOOL ASBEqual(id left, id right) {
    if (boolean(left) != boolean(right)) return NO;
    if ([left isKindOfClass:NSDictionary.class] && [right isKindOfClass:NSDictionary.class]) {
        if (![[NSSet setWithArray:[left allKeys]] isEqual:[NSSet setWithArray:[right allKeys]]]) return NO;
        for (id key in left) if (!ASBEqual(left[key], right[key])) return NO; return YES;
    }
    if ([left isKindOfClass:NSArray.class] && [right isKindOfClass:NSArray.class]) {
        if ([left count] != [right count]) return NO;
        for (NSUInteger i = 0; i < [left count]; i++) if (!ASBEqual(left[i], right[i])) return NO; return YES;
    }
    return left == right || [left isEqual:right];
}
NSArray *ASBDifferences(id expected, id actual, NSString *path) {
    if (ASBEqual(expected, actual)) return @[];
    NSMutableArray *result = NSMutableArray.array;
    if ([expected isKindOfClass:NSDictionary.class] && [actual isKindOfClass:NSDictionary.class] && [[NSSet setWithArray:[expected allKeys]] isEqual:[NSSet setWithArray:[actual allKeys]]]) {
        for (NSString *key in expected) [result addObjectsFromArray:ASBDifferences(expected[key], actual[key], [path stringByAppendingFormat:@"/%@", key])];
    } else if ([expected isKindOfClass:NSArray.class] && [actual isKindOfClass:NSArray.class] && [expected count] == [actual count]) {
        for (NSUInteger i = 0; i < [expected count]; i++) [result addObjectsFromArray:ASBDifferences(expected[i], actual[i], [path stringByAppendingFormat:@"/%lu", (unsigned long)i])];
    } else [result addObject:@{@"path":path, @"expected":expected ?: NSNull.null, @"actual":actual ?: NSNull.null}];
    return result;
}
NSArray *ASBResultDifferences(id expected, id actual, NSString *path) {
    if (ASBEqual(expected, actual)) return @[];
    if ([expected isKindOfClass:NSDictionary.class] && [actual isKindOfClass:NSDictionary.class] &&
        [[NSSet setWithArray:[expected allKeys]] isEqual:[NSSet setWithArray:[actual allKeys]]]) {
        NSDictionary *counts = @{@"color":@4, @"point":@2, @"size":@2, @"rect":@4};
        NSUInteger count = [counts[expected[@"type"] ?: @""] unsignedIntegerValue];
        BOOL close = count && [expected[@"type"] isEqual:actual[@"type"]] &&
            [expected[@"value"] isKindOfClass:NSArray.class] && [actual[@"value"] isKindOfClass:NSArray.class] &&
            [expected[@"value"] count] == count && [actual[@"value"] count] == count;
        if (close) {
            for (NSUInteger i = 0; i < count; i++) {
                id a = expected[@"value"][i], b = actual[@"value"][i];
                if (![a isKindOfClass:NSNumber.class] || ![b isKindOfClass:NSNumber.class] || boolean(a) || boolean(b)) { close = NO; break; }
                double x = [a doubleValue], y = [b doubleValue];
                if (!isfinite(x) || !isfinite(y) || fabs(x - y) > 32 * DBL_EPSILON * fmax(1, fmax(fabs(x), fabs(y)))) { close = NO; break; }
            }
        }
        NSMutableArray *result = NSMutableArray.array;
        for (NSString *key in expected) if (!(close && [key isEqual:@"value"]))
            [result addObjectsFromArray:ASBResultDifferences(expected[key], actual[key], [path stringByAppendingFormat:@"/%@", key])];
        return result;
    }
    if ([expected isKindOfClass:NSArray.class] && [actual isKindOfClass:NSArray.class] && [expected count] == [actual count]) {
        NSMutableArray *result = NSMutableArray.array;
        for (NSUInteger i = 0; i < [expected count]; i++) [result addObjectsFromArray:ASBResultDifferences(expected[i], actual[i], [path stringByAppendingFormat:@"/%lu", (unsigned long)i])];
        return result;
    }
    return ASBDifferences(expected, actual, path);
}
NSArray *ASBCases(void) {
    NSArray *cases = ASBLoad([ASBDirectory stringByAppendingPathComponent:@"cases.json"]);
    NSMutableSet *identifiers = NSMutableSet.set;
    for (NSDictionary *test in cases) {
        NSString *name = test[@"id"];
        ASBRequire(name.length && ![identifiers containsObject:name] && [test[@"operations"] isKindOfClass:NSArray.class], @"Invalid or duplicate test case");
        [identifiers addObject:name];
    }
    return cases;
}
@implementation ASBAPI
- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSString *home = NSHomeDirectory(), *user = NSProcessInfo.processInfo.environment[@"SUDO_USER"];
    struct passwd *owner = user.length ? getpwnam(user.UTF8String) : NULL;
    if (owner) home = @(owner->pw_dir);
    _support = [home stringByAppendingPathComponent:@"Library/Application Support/AppSandbox"];
    _discovery = ASBLoad([_support stringByAppendingPathComponent:@"host.json"]);
    NSURL *url = [NSURL URLWithString:_discovery[@"endpoint"]];
    ASBRequire([url.scheme isEqual:@"http"] && [@[@"127.0.0.1", @"localhost", @"::1", @"[::1]"] containsObject:url.host] && [_discovery[@"token"] length], @"Invalid local headless API discovery");
    return self;
}
- (id)request:(NSString *)method path:(NSString *)path {
    NSString *endpoint = self.discovery[@"endpoint"];
    while ([endpoint hasSuffix:@"/"]) endpoint = [endpoint substringToIndex:endpoint.length - 1];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[endpoint stringByAppendingFormat:@"/v1%@", path]] cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:60];
    request.HTTPMethod = method; [request setValue:[@"Bearer " stringByAppendingString:self.discovery[@"token"]] forHTTPHeaderField:@"Authorization"];
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block NSData *body; __block NSError *error; __block NSInteger status;
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *failure) {
        body = data; error = failure; status = [(NSHTTPURLResponse *)response statusCode]; dispatch_semaphore_signal(finished);
    }];
    [task resume];
    if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 61 * NSEC_PER_SEC))) { [task cancel]; ASBRequire(NO, @"Headless API deadline"); }
    ASBRequire(!error && status >= 200 && status < 300, [NSString stringWithFormat:@"Headless API %@ %@ failed (%ld): %@", method, path, (long)status, error.localizedDescription ?: @"request rejected"]);
    return body.length ? ASBParse(body) : @{};
}
- (NSString *)vmPath:(NSString *)name {
    NSCharacterSet *safe = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~"];
    NSDictionary *vm = [self request:@"GET" path:[@"/vms/" stringByAppendingString:[name stringByAddingPercentEncodingWithAllowedCharacters:safe]]];
    ASBRequire([vm[@"id"] length], @"VM identity missing"); return [@"/vm-ids/" stringByAppendingString:[vm[@"id"] stringByAddingPercentEncodingWithAllowedCharacters:safe]];
}
- (NSDictionary *)status:(NSString *)name { return [self request:@"GET" path:[self vmPath:name]]; }
@end
