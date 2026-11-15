#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <math.h>

static CFMutableDictionaryRef identities;
static NSMutableArray *elements, *issues;
static NSUInteger nodeLimit, valueCount;
static double deadline;

static double now(void) { return NSProcessInfo.processInfo.systemUptime; }
static BOOL element(id value) {
    return value && CFGetTypeID((__bridge CFTypeRef)value) == AXUIElementGetTypeID();
}
static void issue(NSString *reason) {
    if (![issues containsObject:reason]) [issues addObject:reason];
}
static BOOL inTime(void) {
    if (now() < deadline) return YES;
    issue(@"deadline exceeded");
    return NO;
}
static NSNumber *identity(id value, BOOL enqueue) {
    NSNumber *known = (__bridge NSNumber *)CFDictionaryGetValue(identities, (__bridge CFTypeRef)value);
    if (known || !enqueue) return known;
    if (elements.count >= nodeLimit) { issue(@"node limit exceeded"); return nil; }
    NSNumber *key = @(elements.count);
    CFDictionarySetValue(identities, (__bridge CFTypeRef)value, (__bridge CFTypeRef)key);
    [elements addObject:value];
    return key;
}
static NSDictionary *errorValue(AXError error) {
    if (error != kAXErrorAttributeUnsupported && error != kAXErrorNoValue && error != kAXErrorNotImplemented)
        issue([NSString stringWithFormat:@"AX getter error %d", error]);
    return @{@"error": @(error)};
}
static NSDictionary *encode(id value, NSUInteger depth) {
    if (++valueCount > 1000000 || depth > 16) { issue(@"value limit exceeded"); return @{@"unread": @YES}; }
    if (!value) return @{@"value": NSNull.null};
    if (element(value)) {
        NSNumber *key = identity(value, NO);
        return key ? @{@"ref": key} : @{@"externalReference": @YES};
    }
    if ([value isKindOfClass:NSString.class]) {
        if ([value length] > 1048576) { issue(@"string limit exceeded"); return @{@"unread": @YES}; }
        return @{@"type": @"string", @"value": value};
    }
    if ([value isKindOfClass:NSNumber.class]) {
        if (!isfinite([value doubleValue])) { issue(@"nonfinite value"); return @{@"unread": @YES}; }
        NSString *kind = CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ? @"boolean" :
            (CFNumberIsFloatType((__bridge CFNumberRef)value) ? @"double" : @"integer");
        return @{@"type": kind, @"value": value};
    }
    if (CFGetTypeID((__bridge CFTypeRef)value) == AXValueGetTypeID()) {
        AXValueRef native = (__bridge AXValueRef)value;
        switch (AXValueGetType(native)) {
            case kAXValueAXErrorType: {
                AXError error;
                if (AXValueGetValue(native, kAXValueAXErrorType, &error)) return errorValue(error);
                break;
            }
            case kAXValueCGPointType: {
                CGPoint point;
                if (AXValueGetValue(native, kAXValueCGPointType, &point) && isfinite(point.x) && isfinite(point.y))
                    return @{@"type": @"point", @"value": @[@(point.x), @(point.y)]};
                break;
            }
            case kAXValueCGSizeType: {
                CGSize size;
                if (AXValueGetValue(native, kAXValueCGSizeType, &size) && isfinite(size.width) && isfinite(size.height))
                    return @{@"type": @"size", @"value": @[@(size.width), @(size.height)]};
                break;
            }
            case kAXValueCFRangeType: {
                CFRange range;
                if (AXValueGetValue(native, kAXValueCFRangeType, &range))
                    return @{@"type": @"range", @"value": @[@(range.location), @(range.length)]};
                break;
            }
            default: break;
        }
    }
    if ([value isKindOfClass:NSArray.class]) {
        NSMutableArray *items = NSMutableArray.array;
        if ([value count] > 65536) { issue(@"array limit exceeded"); return @{@"unread": @YES}; }
        for (id child in value) [items addObject:encode(child, depth + 1)];
        return @{@"items": items};
    }
    if ([value isKindOfClass:NSURL.class]) return @{@"type": @"url", @"value": [value absoluteString]};
    if ([value isKindOfClass:NSDate.class]) return @{@"type": @"date", @"value": @([value timeIntervalSince1970])};
    return @{@"unsupportedType": @(CFGetTypeID((__bridge CFTypeRef)value))};
}
static id readAttribute(AXUIElementRef target, NSString *attribute, AXError *error) {
    if (!inTime()) { *error = kAXErrorCannotComplete; return nil; }
    CFTypeRef value = NULL;
    *error = AXUIElementCopyAttributeValue(target, (__bridge CFStringRef)attribute, &value);
    return CFBridgingRelease(value);
}
static NSDictionary *names(AXUIElementRef target, BOOL actions) {
    if (!inTime()) return @{@"unread": @YES};
    CFArrayRef values = NULL;
    AXError error = actions ? AXUIElementCopyActionNames(target, &values) :
        AXUIElementCopyParameterizedAttributeNames(target, &values);
    NSArray *array = CFBridgingRelease(values);
    return error ? errorValue(error) : @{@"value": [array sortedArrayUsingSelector:@selector(compare:)] ?: @[]};
}
static NSDictionary *secureValue(id value) {
    if ([value isKindOfClass:NSString.class]) {
        NSCharacterSet *masks = [NSCharacterSet characterSetWithCharactersInString:@"•●*∙◦\uf79a"];
        if ([value rangeOfCharacterFromSet:masks.invertedSet].location == NSNotFound) return encode(value, 0);
    }
    return @{@"redacted": @YES};
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr, "Usage: ax-tree PID MAX_NODES TIMEOUT_SECONDS ATTRIBUTES_CSV WINDOW_TITLE\n");
            return 64;
        }
        int pid = [@(argv[1]) intValue];
        nodeLimit = [@(argv[2]) integerValue];
        double timeout = [@(argv[3]) doubleValue];
        if (pid <= 0 || nodeLimit < 1 || nodeLimit > 65536 || !isfinite(timeout) || timeout <= 0 || timeout > 120) return 64;
        double start = now();
        deadline = start + timeout;
        issues = NSMutableArray.array;
        elements = NSMutableArray.array;
        identities = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        NSMutableOrderedSet *fields = [NSMutableOrderedSet orderedSetWithArray:[@(argv[4]) componentsSeparatedByString:@","]];
        [fields addObjectsFromArray:@[@"AXRole", @"AXSubrole", @"AXProtectedContent"]];
        if (fields.count > 128) return 64;
        for (NSString *field in fields) if (![field hasPrefix:@"AX"] || field.length > 128) return 64;
        BOOL trusted = AXIsProcessTrusted();
        if (!trusted) issue(@"Accessibility permission is required for the invoking terminal or reader");
        AXUIElementRef app = AXUIElementCreateApplication(pid);
        AXError timeoutError = AXUIElementSetMessagingTimeout(app, 0.25);
        if (timeoutError) issue(@"could not set AX messaging timeout");
        AXError windowsError = kAXErrorAPIDisabled;
        id windows = trusted ? readAttribute(app, @"AXWindows", &windowsError) : nil;
        NSMutableArray *roots = NSMutableArray.array, *nodes = NSMutableArray.array;
        if (windowsError) { errorValue(windowsError); issue(@"could not read application windows"); }
        else if (![windows isKindOfClass:NSArray.class]) issue(@"invalid application windows");
        else for (id window in windows) {
            if (!element(window)) { issue(@"invalid window element"); continue; }
            if (strlen(argv[5])) {
                AXError titleError;
                id title = readAttribute((__bridge AXUIElementRef)window, @"AXTitle", &titleError);
                if (titleError) { errorValue(titleError); continue; }
                if (![title isEqual:@(argv[5])]) continue;
            }
            NSNumber *key = identity(window, YES);
            if (key) [roots addObject:key];
        }
        if (!roots.count || (strlen(argv[5]) && roots.count != 1)) issue(@"no unique matching window");
        NSMutableArray *rawRecords = NSMutableArray.array;
        for (NSUInteger index = 0; index < elements.count && inTime(); index++) {
            @autoreleasepool {
                AXUIElementRef target = (__bridge AXUIElementRef)elements[index];
                if (AXUIElementSetMessagingTimeout(target, 0.25)) issue(@"could not set element messaging timeout");
                AXError childrenError;
                id children = readAttribute(target, @"AXChildren", &childrenError);
                NSMutableArray *childIDs = NSMutableArray.array;
                if (childrenError) errorValue(childrenError);
                else if (![children isKindOfClass:NSArray.class]) issue(@"invalid children array");
                else for (id child in children) {
                    if (!element(child)) { issue(@"invalid child element"); continue; }
                    NSNumber *key = identity(child, YES);
                    if (key) [childIDs addObject:key];
                }
                CFArrayRef values = NULL;
                AXError error = inTime() ? AXUIElementCopyMultipleAttributeValues(target,
                    (__bridge CFArrayRef)fields.array, 0, &values) : kAXErrorCannotComplete;
                NSArray *raw = CFBridgingRelease(values);
                NSMutableDictionary *attributes = NSMutableDictionary.dictionary;
                if (error) errorValue(error);
                else if (raw.count != fields.count) { issue(@"invalid batched attribute count"); error = kAXErrorFailure; }
                for (NSUInteger n = 0; n < fields.count; n++) attributes[fields[n]] = error ? errorValue(error) : raw[n];
                NSMutableDictionary *node = [@{@"id": @(index), @"children": childIDs,
                    @"childrenError": @(childrenError), @"attributes": attributes,
                    @"actions": names(target, YES), @"queries": names(target, NO)} mutableCopy];
                [rawRecords addObject:node];
            }
        }
        for (NSMutableDictionary *node in rawRecords) {
            NSDictionary *raw = node[@"attributes"];
            BOOL secure = [raw[@"AXSubrole"] isEqual:@"AXSecureTextField"] || [raw[@"AXProtectedContent"] isEqual:@YES];
            NSMutableDictionary *attributes = NSMutableDictionary.dictionary;
            for (NSString *field in fields) {
                id value = raw[field];
                if ([value isKindOfClass:NSDictionary.class] && value[@"error"]) attributes[field] = value;
                else if (secure && ([field isEqual:@"AXValue"] || [field containsString:@"Text"] || [field containsString:@"Character"])) {
                    if (CFGetTypeID((__bridge CFTypeRef)value) == AXValueGetTypeID() && AXValueGetType((__bridge AXValueRef)value) == kAXValueAXErrorType)
                        attributes[field] = encode(value, 0);
                    else attributes[field] = secureValue(value);
                } else attributes[field] = encode(value, 0);
            }
            if (!attributes[@"AXRole"][@"value"]) issue(@"node role unavailable");
            node[@"attributes"] = attributes;
            [nodes addObject:node];
        }
        if (nodes.count != elements.count) issue(@"unread nodes");
        inTime();
        NSRunningApplication *application = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        NSDictionary *result = @{@"schema": @"asb-ax-tree-1", @"api": @"macOS AXUIElement", @"readOnly": @YES,
            @"pid": @(pid), @"executable": application.executableURL.path ?: @"",
            @"processStartedAt": application.launchDate.description ?: @"", @"capturedAt": NSDate.date.description,
            @"scope": @"AXWindows/AXChildren", @"windowTitle": @(argv[5]), @"trusted": @(trusted),
            @"complete": @((BOOL)(issues.count == 0)), @"issues": issues, @"elapsedMs": @((now() - start) * 1000),
            @"limits": @{@"nodes": @(nodeLimit), @"seconds": @(timeout)}, @"fields": fields.array,
            @"roots": roots, @"nodes": nodes};
        NSError *jsonError;
        NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingSortedKeys error:&jsonError];
        if (json) { fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout); }
        else fprintf(stderr, "%s\n", jsonError.localizedDescription.UTF8String);
        CFRelease(app);
        CFRelease(identities);
        return json && !issues.count ? 0 : 2;
    }
}
