#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <libproc.h>

static double deadline;
static NSMutableArray *references, *issues, *events;
static NSMutableDictionary *handles;
static NSMutableArray *operationValues;
static id operationValue;
static NSUInteger nextHandle;
static NSUInteger valueCount;
static AXUIElementRef application;
static BOOL expired(void) { return NSProcessInfo.processInfo.systemUptime >= deadline; }
static BOOL sessionAvailable(void) { return ![NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier isEqual:@"com.apple.loginwindow"]; }
static BOOL isElement(id value) { return value && CFGetTypeID((__bridge CFTypeRef)value) == AXUIElementGetTypeID(); }
static id readAX(AXUIElementRef element, NSString *name, AXError *error) {
    CFTypeRef value = NULL;
    AXError code = expired() ? kAXErrorCannotComplete : AXUIElementCopyAttributeValue(element, (__bridge CFStringRef)name, &value);
    if (error) *error = code;
    return CFBridgingRelease(value);
}
static NSDictionary *identity(AXUIElementRef element) {
    NSMutableDictionary *result = NSMutableDictionary.dictionary;
    for (NSString *key in @[@"AXIdentifier", @"AXRole", @"AXSubrole", @"AXTitle", @"AXDescription"])
        if (!expired()) { id value = readAX(element, key, NULL); if ([value isKindOfClass:NSString.class]) result[key] = value; }
    return result;
}
static id encode(id value, unsigned depth) {
    if (++valueCount > 100000 || depth > 24) { [issues addObject:@"value limit"]; return @{@"unread": @YES}; }
    if (!value) return NSNull.null;
    CFTypeID type = CFGetTypeID((__bridge CFTypeRef)value);
    if (isElement(value)) {
        NSUInteger index = [references indexOfObject:value];
        if (index == NSNotFound) { index = references.count; [references addObject:value]; }
        return @{@"type": @"element", @"ref": @(index), @"identity": identity((__bridge AXUIElementRef)value)};
    }
    if (type == AXTextMarkerGetTypeID()) {
        AXTextMarkerRef marker = (__bridge AXTextMarkerRef)value;
        NSData *bytes = [NSData dataWithBytes:AXTextMarkerGetBytePtr(marker) length:AXTextMarkerGetLength(marker)];
        return @{@"type": @"text-marker", @"bytes": [bytes base64EncodedStringWithOptions:0]};
    }
    if (type == AXTextMarkerRangeGetTypeID()) {
        AXTextMarkerRangeRef range = (__bridge AXTextMarkerRangeRef)value;
        id start = CFBridgingRelease(AXTextMarkerRangeCopyStartMarker(range)), end = CFBridgingRelease(AXTextMarkerRangeCopyEndMarker(range));
        return @{@"type": @"text-marker-range", @"start": encode(start, depth + 1), @"end": encode(end, depth + 1)};
    }
    if ([value isKindOfClass:NSString.class]) return @{@"type": @"string", @"value": value};
    if ([value isKindOfClass:NSNumber.class]) {
        NSString *kind = type == CFBooleanGetTypeID() ? @"boolean" : CFNumberIsFloatType((__bridge CFNumberRef)value) ? @"double" : @"integer";
        if (!isfinite([value doubleValue])) return @{@"type": kind, @"nonfinite": isnan([value doubleValue]) ? @"nan" : [value doubleValue] < 0 ? @"-infinity" : @"infinity"};
        return @{@"type": kind, @"value": value};
    }
    if (type == AXValueGetTypeID()) {
        AXValueRef v = (__bridge AXValueRef)value;
        switch (AXValueGetType(v)) {
            case kAXValueCGPointType: { CGPoint p; if (AXValueGetValue(v, kAXValueCGPointType, &p)) return @{@"type": @"point", @"value": @[@(p.x), @(p.y)]}; break; }
            case kAXValueCGSizeType: { CGSize s; if (AXValueGetValue(v, kAXValueCGSizeType, &s)) return @{@"type": @"size", @"value": @[@(s.width), @(s.height)]}; break; }
            case kAXValueCGRectType: { CGRect r; if (AXValueGetValue(v, kAXValueCGRectType, &r)) return @{@"type": @"rect", @"value": @[@(r.origin.x), @(r.origin.y), @(r.size.width), @(r.size.height)]}; break; }
            case kAXValueCFRangeType: { CFRange r; if (AXValueGetValue(v, kAXValueCFRangeType, &r)) return @{@"type": @"range", @"value": @[@(r.location), @(r.length)]}; break; }
            case kAXValueAXErrorType: { AXError e; if (AXValueGetValue(v, kAXValueAXErrorType, &e)) return @{@"type": @"ax-error", @"value": @(e)}; break; }
            default: break;
        }
    }
    if ([value isKindOfClass:NSArray.class]) {
        NSMutableArray *items = NSMutableArray.array;
        for (id item in value) [items addObject:encode(item, depth + 1)];
        return @{@"type": @"array", @"value": items};
    }
    if ([value isKindOfClass:NSDictionary.class]) {
        NSMutableArray *items = NSMutableArray.array;
        for (id key in value) [items addObject:@[encode(key, depth + 1), encode(value[key], depth + 1)]];
        [items sortUsingComparator:^NSComparisonResult(id a, id b) { return [[a[0] description] compare:[b[0] description]]; }];
        return @{@"type": @"dictionary", @"value": items};
    }
    if ([value isKindOfClass:NSData.class]) return @{@"type": @"data", @"value": [value base64EncodedStringWithOptions:0]};
    if ([value isKindOfClass:NSDate.class]) return @{@"type": @"date", @"value": @([value timeIntervalSince1970])};
    if ([value isKindOfClass:NSURL.class]) return @{@"type": @"url", @"value": [value absoluteString]};
    if ([value isKindOfClass:NSAttributedString.class]) {
        NSMutableArray *runs = NSMutableArray.array;
        [value enumerateAttributesInRange:NSMakeRange(0, [value length]) options:0 usingBlock:^(NSDictionary *attributes, NSRange range, BOOL *stop) {
            (void)stop; [runs addObject:@{@"range": @[@(range.location), @(range.length)], @"attributes": encode(attributes, depth + 1)}];
        }];
        return @{@"type": @"attributed-string", @"value": [value string], @"runs": runs};
    }
    if ([value isKindOfClass:NSFont.class]) return @{@"type": @"font", @"name": [value fontName], @"size": @([value pointSize])};
    if ([value isKindOfClass:NSColor.class] || type == CGColorGetTypeID()) {
        NSColor *source = type == CGColorGetTypeID() ? [NSColor colorWithCGColor:(__bridge CGColorRef)value] : value;
        NSColor *color = [source colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
        if (color) return @{@"type": @"color", @"value": @[@(color.redComponent), @(color.greenComponent), @(color.blueComponent), @(color.alphaComponent)]};
    }
    [issues addObject:[NSString stringWithFormat:@"unsupported value type %lu", (unsigned long)type]];
    return @{@"unsupportedType": @(type)};
}
static id decode(id value) {
    if (![value isKindOfClass:NSDictionary.class]) return value == NSNull.null ? nil : value;
    NSString *type = value[@"type"]; id v = value[@"value"];
    if ([type isEqual:@"result"] && value[@"index"] && [value[@"index"] unsignedIntegerValue] < operationValues.count) {
        id result = operationValues[[value[@"index"] unsignedIntegerValue]];
        return result == NSNull.null ? nil : result;
    }
    if ([type isEqual:@"clone-marker"]) {
        id marker = decode(value[@"marker"]);
        if (marker && CFGetTypeID((__bridge CFTypeRef)marker) == AXTextMarkerGetTypeID())
            return CFBridgingRelease(AXTextMarkerCreate(NULL, AXTextMarkerGetBytePtr((__bridge AXTextMarkerRef)marker), AXTextMarkerGetLength((__bridge AXTextMarkerRef)marker)));
        return nil;
    }
    if ([type isEqual:@"marker-start"] || [type isEqual:@"marker-end"]) {
        id range = decode(value[@"range"]);
        if (!range || CFGetTypeID((__bridge CFTypeRef)range) != AXTextMarkerRangeGetTypeID()) return nil;
        return CFBridgingRelease([type isEqual:@"marker-start"] ? AXTextMarkerRangeCopyStartMarker((__bridge AXTextMarkerRangeRef)range) : AXTextMarkerRangeCopyEndMarker((__bridge AXTextMarkerRangeRef)range));
    }
    if ([type isEqual:@"offset"]) {
        id base = decode(value[@"base"]), amount = value[@"by"];
        if (![base isKindOfClass:NSNumber.class] || ![amount isKindOfClass:NSNumber.class]) return nil;
        long long result;
        return __builtin_add_overflow([base longLongValue], [amount longLongValue], &result) ? nil : @(result);
    }
    if ([type isEqual:@"marker-range"]) {
        id start = decode(value[@"start"]), end = decode(value[@"end"]);
        if (!start || !end || CFGetTypeID((__bridge CFTypeRef)start) != AXTextMarkerGetTypeID() || CFGetTypeID((__bridge CFTypeRef)end) != AXTextMarkerGetTypeID()) return nil;
        AXTextMarkerRef a = (__bridge AXTextMarkerRef)start, b = (__bridge AXTextMarkerRef)end;
        return CFBridgingRelease([value[@"fromBytes"] boolValue] ? AXTextMarkerRangeCreateWithBytes(NULL, AXTextMarkerGetBytePtr(a), AXTextMarkerGetLength(a), AXTextMarkerGetBytePtr(b), AXTextMarkerGetLength(b)) : AXTextMarkerRangeCreate(NULL, a, b));
    }
    if ([type isEqual:@"center"]) {
        id rect = decode(value[@"rect"]); CGRect bounds;
        if (rect && CFGetTypeID((__bridge CFTypeRef)rect) == AXValueGetTypeID() && AXValueGetValue((__bridge AXValueRef)rect, kAXValueCGRectType, &bounds)) {
            CGPoint point = CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds)); return CFBridgingRelease(AXValueCreate(kAXValueCGPointType, &point));
        }
        return nil;
    }
    if ([type isEqual:@"string"] && [v isKindOfClass:NSString.class]) return v;
    if ([type isEqual:@"boolean"] && [v isKindOfClass:NSNumber.class]) return [v boolValue] ? @YES : @NO;
    if ([type isEqual:@"integer"] && [v isKindOfClass:NSNumber.class]) return @([v longLongValue]);
    if ([type isEqual:@"double"] && [v isKindOfClass:NSNumber.class]) return @([v doubleValue]);
    if ([type isEqual:@"url"] && [v isKindOfClass:NSString.class]) return [NSURL URLWithString:v];
    if ([type isEqual:@"date"] && [v isKindOfClass:NSNumber.class]) return [NSDate dateWithTimeIntervalSince1970:[v doubleValue]];
    if ([type isEqual:@"data"] && [v isKindOfClass:NSString.class]) return [[NSData alloc] initWithBase64EncodedString:v options:0];
    if ([type isEqual:@"element"] && value[@"ref"] && [value[@"ref"] unsignedIntegerValue] < references.count) return references[[value[@"ref"] unsignedIntegerValue]];
    if ([type isEqual:@"array"] && [v isKindOfClass:NSArray.class]) {
        NSMutableArray *result = NSMutableArray.array;
        for (id item in v) { id decoded = decode(item); if (!decoded) return nil; [result addObject:decoded]; }
        return result;
    }
    if ([type isEqual:@"dictionary"] && [v isKindOfClass:NSArray.class]) {
        NSMutableDictionary *result = NSMutableDictionary.dictionary;
        for (id pair in v) {
            if (![pair isKindOfClass:NSArray.class] || [pair count] != 2) return nil;
            id key = decode(pair[0]), item = decode(pair[1]); if (!key || !item || ![key conformsToProtocol:@protocol(NSCopying)]) return nil;
            result[key] = item;
        }
        return result;
    }
    if ([v isKindOfClass:NSArray.class]) {
        for (id n in v) if (![n isKindOfClass:NSNumber.class] || !isfinite([n doubleValue])) return nil;
        if ([type isEqual:@"range"] && [v count] == 2) { CFRange r = CFRangeMake([v[0] longValue], [v[1] longValue]); return CFBridgingRelease(AXValueCreate(kAXValueCFRangeType, &r)); }
        if ([type isEqual:@"point"] && [v count] == 2) { CGPoint p = CGPointMake([v[0] doubleValue], [v[1] doubleValue]); return CFBridgingRelease(AXValueCreate(kAXValueCGPointType, &p)); }
        if ([type isEqual:@"size"] && [v count] == 2) { CGSize s = CGSizeMake([v[0] doubleValue], [v[1] doubleValue]); return CFBridgingRelease(AXValueCreate(kAXValueCGSizeType, &s)); }
        if ([type isEqual:@"rect"] && [v count] == 4) { CGRect r = CGRectMake([v[0] doubleValue], [v[1] doubleValue], [v[2] doubleValue], [v[3] doubleValue]); return CFBridgingRelease(AXValueCreate(kAXValueCGRectType, &r)); }
    }
    return nil;
}
static id resolve(AXUIElementRef app, NSDictionary *request, NSMutableDictionary *report) {
    if ([request[@"scope"] isEqual:@"application"]) return (__bridge id)app;
    NSMutableArray *queue = NSMutableArray.array, *matches = NSMutableArray.array;
    NSMutableSet *seen = NSMutableSet.set;
    AXError error; id windows = readAX(app, @"AXWindows", &error);
    if (error || ![windows isKindOfClass:NSArray.class]) { report[@"lookupError"] = @(error ?: kAXErrorFailure); return nil; }
    for (id window in windows)
        if (!request[@"window"] || [readAX((__bridge AXUIElementRef)window, @"AXTitle", NULL) isEqual:request[@"window"]]) [queue addObject:window];
    if (request[@"under"]) {
        NSMutableArray *roots = NSMutableArray.array;
        for (id window in queue) {
            id children = readAX((__bridge AXUIElementRef)window, @"AXChildren", NULL);
            if (![children isKindOfClass:NSArray.class]) continue;
            for (id child in children) {
                if (!isElement(child)) continue;
                BOOL match = YES;
                for (NSString *key in request[@"under"]) if (![readAX((__bridge AXUIElementRef)child, key, NULL) isEqual:request[@"under"][key]]) { match = NO; break; }
                if (match) [roots addObject:child];
            }
        }
        if (roots.count != 1) { report[@"scopeMatches"] = @(roots.count); return nil; }
        queue = roots;
    }
    if ([request[@"menus"] boolValue]) { id menu = readAX(app, @"AXMenuBar", NULL); if (isElement(menu)) [queue addObject:menu]; }
    NSDictionary *selector = request[@"target"];
    if (![selector isKindOfClass:NSDictionary.class] || !selector.count) return nil;
    NSUInteger maximum = request[@"maxNodes"] ? MIN(65536, [request[@"maxNodes"] unsignedIntegerValue]) : 8192;
    NSMutableDictionary *index = NSMutableDictionary.dictionary;
    NSMapTable *parents = [NSMapTable strongToStrongObjectsMapTable];
    for (NSUInteger i = 0; i < queue.count; i++) {
        if (expired() || seen.count >= maximum) { report[@"lookupIncomplete"] = @YES; break; }
        id item = queue[i]; if ([seen containsObject:item]) continue; [seen addObject:item];
        AXUIElementRef element = (__bridge AXUIElementRef)item; AXUIElementSetMessagingTimeout(element, .5);
        id identifier = ([request[@"retainIndex"] boolValue] || [request[@"excludeSubtrees"] count]) ? readAX(element, @"AXIdentifier", NULL) : nil;
        if ([request[@"retainIndex"] boolValue]) {
            if ([identifier isKindOfClass:NSString.class] && [identifier length]) {
                if (!index[identifier]) index[identifier] = NSMutableArray.array;
                [index[identifier] addObject:item];
            }
        }
        BOOL match = YES;
        for (NSString *key in selector) if (![readAX(element, key, NULL) isEqual:selector[key]]) { match = NO; break; }
        if (match) [matches addObject:item];
        if (identifier && [request[@"excludeSubtrees"] containsObject:identifier]) {
            if (!report[@"excludedSubtrees"]) report[@"excludedSubtrees"] = NSMutableArray.array;
            [report[@"excludedSubtrees"] addObject:identifier]; continue;
        }
        id children = readAX(element, @"AXChildren", &error);
        if (error && error != kAXErrorNoValue && error != kAXErrorAttributeUnsupported && error != kAXErrorNotImplemented) {
            report[@"lookupIncomplete"] = @YES;
            if (!report[@"lookupErrors"]) report[@"lookupErrors"] = NSMutableArray.array;
            if ([report[@"lookupErrors"] count] < 32) [report[@"lookupErrors"] addObject:@{@"error":@(error), @"element":encode(item, 0), @"parent":encode([parents objectForKey:item], 0)}];
        }
        if ([children isKindOfClass:NSArray.class]) for (id child in children) if (isElement(child)) { [queue addObject:child]; [parents setObject:item forKey:child]; }
    }
    report[@"matches"] = @(matches.count); report[@"scanned"] = @(seen.count);
    if (![report[@"lookupIncomplete"] boolValue] && index.count) {
        NSMutableArray *indexed = NSMutableArray.array;
        for (NSString *identifier in index) if ([index[identifier] count] == 1) {
            NSString *handle = [NSString stringWithFormat:@"%lu", (unsigned long)++nextHandle];
            handles[handle] = @{@"element": index[identifier][0], @"pid": request[@"pid"], @"executable": report[@"executable"]};
            [indexed addObject:@{@"selector": @{@"AXIdentifier": identifier}, @"handle": handle}];
        }
        report[@"indexed"] = indexed;
    }
    return matches.count == 1 && ![report[@"lookupIncomplete"] boolValue] ? matches[0] : nil;
}
static NSDictionary *operation(AXUIElementRef element, NSDictionary *op) {
    double start = NSProcessInfo.processInfo.systemUptime;
    NSString *kind = op[@"kind"], *name = op[@"name"];
    AXError error = kAXErrorIllegalArgument;
    id value = nil; CFTypeRef copied = NULL;
    NSMutableDictionary *result = [@{@"operation": op} mutableCopy];
    if (op[@"targetRef"]) {
        NSUInteger index = [op[@"targetRef"] unsignedIntegerValue];
        if (index >= references.count) return @{@"operation": op, @"error": @(kAXErrorIllegalArgument)};
        element = (__bridge AXUIElementRef)references[index];
    }
    if (expired()) error = kAXErrorCannotComplete;
    else if ([kind isEqual:@"get"]) value = readAX(element, name, &error);
    else if ([kind isEqual:@"settable"]) { Boolean writable = false; error = AXUIElementIsAttributeSettable(element, (__bridge CFStringRef)name, &writable); value = writable ? @YES : @NO; }
    else if ([kind isEqual:@"set"]) { id decoded = decode(op[@"value"]); if (decoded) error = AXUIElementSetAttributeValue(element, (__bridge CFStringRef)name, (__bridge CFTypeRef)decoded); }
    else if ([kind isEqual:@"action"]) error = AXUIElementPerformAction(element, (__bridge CFStringRef)name);
    else if ([kind isEqual:@"query"]) { id decoded = decode(op[@"parameter"]); if (decoded) error = AXUIElementCopyParameterizedAttributeValue(element, (__bridge CFStringRef)name, (__bridge CFTypeRef)decoded, &copied); }
    else if ([kind isEqual:@"action-description"]) error = AXUIElementCopyActionDescription(element, (__bridge CFStringRef)name, (CFStringRef *)&copied);
    else if ([kind isEqual:@"count"]) { CFIndex count = 0; error = AXUIElementGetAttributeValueCount(element, (__bridge CFStringRef)name, &count); value = @(count); }
    else if ([kind isEqual:@"slice"]) error = AXUIElementCopyAttributeValues(element, (__bridge CFStringRef)name, [op[@"index"] longValue], [op[@"count"] longValue], (CFArrayRef *)&copied);
    else if ([kind isEqual:@"multiple"]) error = AXUIElementCopyMultipleAttributeValues(element, (__bridge CFArrayRef)op[@"names"], [op[@"stopOnError"] boolValue] ? kAXCopyMultipleAttributeOptionStopOnError : 0, (CFArrayRef *)&copied);
    else if ([kind isEqual:@"attributes"]) error = AXUIElementCopyAttributeNames(element, (CFArrayRef *)&copied);
    else if ([kind isEqual:@"actions"]) error = AXUIElementCopyActionNames(element, (CFArrayRef *)&copied);
    else if ([kind isEqual:@"queries"]) error = AXUIElementCopyParameterizedAttributeNames(element, (CFArrayRef *)&copied);
    else if ([kind isEqual:@"hit"]) error = AXUIElementCopyElementAtPosition(element, [op[@"x"] doubleValue], [op[@"y"] doubleValue], (AXUIElementRef *)&copied);
    else if ([kind isEqual:@"hit-center"]) {
        id position = readAX(element, @"AXPosition", &error), size = readAX(element, @"AXSize", &error);
        CGPoint point; CGSize bounds;
        if (position && size && CFGetTypeID((__bridge CFTypeRef)position) == AXValueGetTypeID() && CFGetTypeID((__bridge CFTypeRef)size) == AXValueGetTypeID() &&
            AXValueGetValue((__bridge AXValueRef)position, kAXValueCGPointType, &point) && AXValueGetValue((__bridge AXValueRef)size, kAXValueCGSizeType, &bounds))
            error = AXUIElementCopyElementAtPosition(application, point.x + bounds.width / 2, point.y + bounds.height / 2, (AXUIElementRef *)&copied);
        else error = kAXErrorNoValue;
    }
    else if ([kind isEqual:@"discover"]) {
        CFArrayRef names = NULL; error = AXUIElementCopyAttributeNames(element, &names);
        NSArray *attributes = CFBridgingRelease(names);
        NSMutableArray *observations = NSMutableArray.array;
        for (NSString *attribute in attributes) {
            NSMutableDictionary *entry = [@{@"name": attribute, @"settable": operation(element, @{@"kind": @"settable", @"name": attribute})} mutableCopy];
            if ([op[@"read"] boolValue]) entry[@"read"] = operation(element, @{@"kind": @"get", @"name": attribute});
            [observations addObject:entry];
        }
        result[@"attributes"] = observations;
        result[@"actions"] = operation(element, @{@"kind": @"actions"});
        result[@"queries"] = operation(element, @{@"kind": @"queries"});
    }
    if (copied) value = CFBridgingRelease(copied);
    operationValue = value;
    result[@"error"] = @(error);
    if (value) result[@"value"] = encode(value, 0);
    result[@"elapsedMs"] = @((NSProcessInfo.processInfo.systemUptime - start) * 1000);
    return result;
}
static void observed(AXObserverRef observer, AXUIElementRef element, CFStringRef notification, CFDictionaryRef info, void *context) {
    (void)observer; (void)context;
    if (events.count >= 10000) { [issues addObject:@"event limit"]; return; }
    [events addObject:@{@"name": (__bridge NSString *)notification, @"element": encode((__bridge id)element, 0),
        @"info": encode((__bridge id)info, 0), @"uptime": @(NSProcessInfo.processInfo.systemUptime)}];
}
static int process(NSData *data) { @autoreleasepool {
    NSError *parse = nil;
    NSDictionary *request = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&parse] : nil;
    if (![request isKindOfClass:NSDictionary.class] || [request[@"pid"] intValue] <= 0 || ![request[@"operations"] isKindOfClass:NSArray.class] || [request[@"operations"] count] > 512) return 64;
    double seconds = request[@"budgetSeconds"] ? [request[@"budgetSeconds"] doubleValue] : 20;
    if (!isfinite(seconds) || seconds <= 0 || seconds > 120) return 64;
    double start = NSProcessInfo.processInfo.systemUptime; deadline = start + seconds;
    references = NSMutableArray.array; issues = NSMutableArray.array; events = NSMutableArray.array; operationValues = NSMutableArray.array; valueCount = 0;
    pid_t pid = [request[@"pid"] intValue]; char path[PROC_PIDPATHINFO_MAXSIZE] = {0}; proc_pidpath(pid, path, sizeof(path));
    NSMutableDictionary *report = [@{@"schema": @"asb-ax-probe-1", @"pid": @(pid), @"executable": @(path), @"trusted": AXIsProcessTrusted() ? @YES : @NO,
        @"capturedAt": NSDate.date.description, @"request": request, @"issues": issues, @"events": events,
        @"frontmostBefore": @(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier)} mutableCopy];
    AXUIElementRef app = AXUIElementCreateApplication(pid); AXUIElementSetMessagingTimeout(app, .5);
    application = app;
    if ([request[@"clearHandles"] boolValue]) [handles removeAllObjects];
    NSDictionary *retained = handles[[request[@"handle"] description]];
    id object = nil;
    report[@"sessionAvailableBefore"] = @(sessionAvailable());
    if (![report[@"sessionAvailableBefore"] boolValue]) [issues addObject:@"host GUI session inactive"];
    if (AXIsProcessTrusted() && [report[@"sessionAvailableBefore"] boolValue]) {
        if (request[@"handle"]) {
            if ([retained[@"pid"] intValue] == pid && [retained[@"executable"] isEqual:@(path)]) object = retained[@"element"];
        } else object = resolve(app, request, report);
    }
    if (!object && !issues.count) [issues addObject:!AXIsProcessTrusted() ? @"accessibility permission required" :
        [report[@"lookupIncomplete"] boolValue] ? @"tree traversal incomplete" : @"target unavailable or ambiguous"];
    AXObserverRef observer = NULL;
    NSMutableArray *registrations = NSMutableArray.array, *results = NSMutableArray.array, *removals = NSMutableArray.array;
    if (object) {
        AXUIElementRef target = (__bridge AXUIElementRef)object;
        [references addObject:object];
        AXUIElementSetMessagingTimeout(target, request[@"messagingTimeout"] ? [request[@"messagingTimeout"] floatValue] : 5);
        if (!request[@"handle"] || [request[@"describeTarget"] boolValue]) report[@"target"] = encode(object, 0);
        if ([request[@"retain"] boolValue]) {
            NSString *handle = [NSString stringWithFormat:@"%lu", (unsigned long)++nextHandle];
            handles[handle] = @{@"element": object, @"pid": @(pid), @"executable": @(path)};
            report[@"handle"] = handle;
        }
        NSArray *notifications = request[@"observe"][@"names"];
        if (notifications.count) {
            pid_t observerPID = pid; AXUIElementGetPid(target, &observerPID); report[@"observerPID"] = @(observerPID);
            AXError created = AXObserverCreateWithInfoCallback(observerPID, observed, &observer); report[@"observerError"] = @(created);
            if (!created) {
                CFRunLoopAddSource(CFRunLoopGetCurrent(), AXObserverGetRunLoopSource(observer), kCFRunLoopDefaultMode);
                for (NSString *name in notifications) [registrations addObject:@{@"name": name, @"error": @(AXObserverAddNotification(observer, target, (__bridge CFStringRef)name, NULL))}];
            }
        }
        for (NSDictionary *op in request[@"operations"]) {
            @try { operationValue = nil; [results addObject:operation(target, op)]; [operationValues addObject:operationValue ?: NSNull.null]; }
            @catch (NSException *exception) { [issues addObject:[@"invalid operation: " stringByAppendingString:exception.name]]; break; }
        }
        if (observer) {
            double duration = MIN(10, MAX(0, [request[@"observe"][@"seconds"] doubleValue]));
            double until = MIN(deadline, NSProcessInfo.processInfo.systemUptime + duration);
            while (NSProcessInfo.processInfo.systemUptime < until) {
                if ([request[@"observe"][@"stopWhenSeen"] boolValue]) {
                    NSMutableSet *seen = NSMutableSet.set; for (NSDictionary *event in events) [seen addObject:event[@"name"]];
                    if ([[NSSet setWithArray:notifications] isSubsetOfSet:seen]) break;
                }
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, MIN(.02, until - NSProcessInfo.processInfo.systemUptime), false);
            }
            for (NSDictionary *registration in registrations) if (![registration[@"error"] intValue])
                [removals addObject:@{@"name": registration[@"name"], @"error": @(AXObserverRemoveNotification(observer, target, (__bridge CFStringRef)registration[@"name"]))}];
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), AXObserverGetRunLoopSource(observer), kCFRunLoopDefaultMode); CFRelease(observer);
        }
    }
    if (expired()) [issues addObject:@"deadline exceeded"];
    report[@"registrations"] = registrations; report[@"removals"] = removals; report[@"results"] = results;
    if ([request[@"release"] boolValue] && request[@"handle"]) [handles removeObjectForKey:[request[@"handle"] description]];
    report[@"frontmostAfter"] = @(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier);
    report[@"sessionAvailableAfter"] = @(sessionAvailable());
    if (![report[@"sessionAvailableAfter"] boolValue] && ![issues containsObject:@"host GUI session inactive"]) [issues addObject:@"host GUI session inactive"];
    report[@"complete"] = issues.count == 0 ? @YES : @NO; report[@"elapsedMs"] = @((NSProcessInfo.processInfo.systemUptime - start) * 1000);
    NSData *encoded = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&parse];
    if (encoded) { fwrite(encoded.bytes, 1, encoded.length, stdout); fputc('\n', stdout); fflush(stdout); }
    CFRelease(app); return encoded && !issues.count ? 0 : 2;
} }
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 64;
    handles = NSMutableDictionary.dictionary;
    if (!strcmp(argv[1], "--serve")) {
        char *line = NULL; size_t capacity = 0; ssize_t length;
        while ((length = getline(&line, &capacity, stdin)) >= 0) {
            @autoreleasepool {
                if (length > 8 * 1024 * 1024) { puts("{\"complete\":false,\"issues\":[\"request limit\"]}"); fflush(stdout); continue; }
                int status = process([NSData dataWithBytes:line length:(NSUInteger)length]);
                if (status == 64) { puts("{\"complete\":false,\"issues\":[\"invalid request\"]}"); fflush(stdout); }
            }
        }
        free(line); return 0;
    }
    NSData *data = !strcmp(argv[1], "-") ? NSFileHandle.fileHandleWithStandardInput.readDataToEndOfFile : [NSData dataWithContentsOfFile:@(argv[1])];
    return process(data);
} }
