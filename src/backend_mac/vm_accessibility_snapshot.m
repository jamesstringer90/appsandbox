#import "vm_accessibility_snapshot.h"
#import "../../tools/transport/accessibility_value_mac.h"

@implementation VmAccessibilitySnapshot
@end

NSDictionary *VmAccessibilityParseRecord(NSDictionary *source) {
    if (![source isKindOfClass:NSDictionary.class]) return nil;
    NSString *nodeID = source[@"id"];
    NSArray *children = source[@"children"], *actions = source[@"actions"], *frame = source[@"frame"];
    if (!VmAccessibilityIsIdentifier(nodeID) || !VmAccessibilityIsString(source[@"role"], ASB_AX_MAX_STRING) ||
        ![source[@"role"] hasPrefix:@"AX"] || ![children isKindOfClass:NSArray.class] ||
        children.count > ASB_AX_MAX_NODES || ![actions isKindOfClass:NSArray.class] || actions.count > 64 ||
        ![frame isKindOfClass:NSArray.class] || frame.count != 4) return nil;
    for (NSUInteger i = 0; i < 4; i++) if (!VmAccessibilityIsNumber(frame[i], i < 2 ? -100000000 : 0, 100000000)) return nil;
    NSMutableDictionary *node = [NSMutableDictionary dictionary];
    node[@"id"] = nodeID;
    node[@"role"] = source[@"role"];
    node[@"frame"] = [frame copy];
    for (NSString *key in @[@"subrole", @"title", @"label"]) {
        if (source[key] && !VmAccessibilityIsString(source[key], ASB_AX_MAX_STRING)) return nil;
        if (source[key]) node[key] = source[key];
    }
    for (NSString *key in @[@"enabled", @"focused", @"writableValue", @"writableFocused", @"writableSelectedTextRange", @"secure"]) {
        if (source[key] && !VmAccessibilityIsInteger(source[key], 0, 1)) return nil;
        node[key] = @([source[key] boolValue]);
    }
    if (source[@"parent"] && !VmAccessibilityIsIdentifier(source[@"parent"])) return nil;
    if (source[@"parent"]) node[@"parent"] = source[@"parent"];
    NSMutableSet *unique = [NSMutableSet set];
    for (id child in children) {
        if (!VmAccessibilityIsIdentifier(child) || [unique containsObject:child]) return nil;
        [unique addObject:child];
    }
    for (id action in actions) if (!VmAccessibilityIsString(action, ASB_AX_MAX_STRING) || ![action length]) return nil;
    node[@"children"] = [children copy];
    node[@"actions"] = [actions copy];
    if (source[@"actionNamesError"]) {
        if (!VmAccessibilityIsInteger(source[@"actionNamesError"], INT32_MIN, -1)) return nil;
        node[@"actionNamesError"] = source[@"actionNamesError"];
    }
    for (NSString *key in @[@"attributeNames", @"parameterizedNames", @"writableAttributes"]) {
        id names = source[key];
        if (names && (![names isKindOfClass:NSArray.class] || [names count] > 512)) return nil;
        for (id name in names) if (!VmAccessibilityIsString(name, 256) || ![name length]) return nil;
        if (names) node[key] = [names copy];
    }
    for (NSString *key in @[@"attributes", @"actionDescriptions", @"settable"]) {
        id values = source[key];
        if (values && (![values isKindOfClass:NSDictionary.class] || !AsbAXWireValueValid(values, 0))) return nil;
        if (values) node[key] = [values copy];
    }
    NSDictionary *attributeErrors = source[@"attributeErrors"];
    if (attributeErrors && (![attributeErrors isKindOfClass:NSDictionary.class] || attributeErrors.count > 512)) return nil;
    for (id name in attributeErrors)
        if (!VmAccessibilityIsString(name, 256) || ![name length] || !VmAccessibilityIsInteger(attributeErrors[name], INT32_MIN, -1)) return nil;
    if (attributeErrors) node[@"attributeErrors"] = [attributeErrors copy];
    id value = source[@"value"];
    if (value && !VmAccessibilityIsString(value, ASB_AX_MAX_STRING) && !VmAccessibilityIsNumber(value, -DBL_MAX, DBL_MAX)) return nil;
    if (value && (![node[@"secure"] boolValue] || AsbAXIsMaskedValue(value))) node[@"value"] = value;
    NSArray *range = source[@"selectedTextRange"];
    if (range) {
        if (![range isKindOfClass:NSArray.class] || range.count != 2 ||
            !VmAccessibilityIsInteger(range[0], 0, 9007199254740991.0) ||
            !VmAccessibilityIsInteger(range[1], 0, 9007199254740991.0)) return nil;
        if (![node[@"secure"] boolValue]) node[@"selectedTextRange"] = [range copy];
    }
    return [node copy];
}

VmAccessibilitySnapshot *VmAccessibilityParseSnapshot(NSDictionary *message) {
    if (!VmAccessibilityIsInteger(message[@"version"], ASB_AX_VERSION, ASB_AX_VERSION) ||
        !VmAccessibilityIsSessionIdentifier(message[@"session"]) || !VmAccessibilityIsInteger(message[@"revision"], 0, 9007199254740991.0)) return nil;
    NSDictionary *app = message[@"app"], *display = message[@"display"];
    NSArray *input = message[@"nodes"], *roots = message[@"roots"];
    if (![app isKindOfClass:NSDictionary.class] || !VmAccessibilityIsInteger(app[@"pid"], 1, INT_MAX) ||
        !VmAccessibilityIsString(app[@"name"], ASB_AX_MAX_STRING) || !VmAccessibilityIsString(app[@"bundleId"], ASB_AX_MAX_STRING) ||
        ![display isKindOfClass:NSDictionary.class] || ![input isKindOfClass:NSArray.class] ||
        input.count > ASB_AX_MAX_NODES || ![roots isKindOfClass:NSArray.class] || roots.count > input.count ||
        (message[@"truncated"] && !VmAccessibilityIsInteger(message[@"truncated"], 0, 1)) ||
        (message[@"refreshGeneration"] && !VmAccessibilityIsInteger(message[@"refreshGeneration"], 0, 9007199254740991.0))) return nil;
    for (NSString *key in @[@"x", @"y"]) if (!VmAccessibilityIsNumber(display[key], -100000000, 100000000)) return nil;
    for (NSString *key in @[@"width", @"height"]) if (!VmAccessibilityIsNumber(display[key], 1, 1000000)) return nil;
    for (NSString *key in @[@"pixelWidth", @"pixelHeight"]) if (!VmAccessibilityIsInteger(display[key], 1, 1000000)) return nil;
    NSMutableDictionary<NSString *, NSMutableDictionary *> *nodes = [NSMutableDictionary dictionaryWithCapacity:input.count];
    for (id item in input) {
        NSDictionary *node = VmAccessibilityParseRecord(item);
        NSString *identifier = node[@"id"];
        if (!node || nodes[identifier]) return nil;
        nodes[identifier] = [node mutableCopy];
    }
    NSMutableDictionary<NSString *, NSString *> *parents = [NSMutableDictionary dictionary];
    NSUInteger edges = 0;
    for (NSString *nodeID in nodes) {
        for (NSString *child in nodes[nodeID][@"children"]) {
            if (!nodes[child] || parents[child] || ++edges > ASB_AX_MAX_NODES) return nil;
            parents[child] = nodeID;
        }
    }
    NSMutableSet *visited = [NSMutableSet set];
    NSMutableArray *queue = [NSMutableArray array];
    for (id root in roots) {
        if (!VmAccessibilityIsIdentifier(root) || !nodes[root] || parents[root] || [visited containsObject:root]) return nil;
        [visited addObject:root];
        [queue addObject:@[root, @0]];
    }
    for (NSUInteger i = 0; i < queue.count; i++) {
        NSString *nodeID = queue[i][0];
        NSUInteger depth = [queue[i][1] unsignedIntegerValue];
        if (depth > 128) return nil;
        for (NSString *child in nodes[nodeID][@"children"]) {
            if ([visited containsObject:child]) return nil;
            [visited addObject:child];
            [queue addObject:@[child, @(depth + 1)]];
        }
    }
    if (visited.count != nodes.count) return nil;
    NSMutableArray *displayRoots = [roots mutableCopy];
    for (NSDictionary *item in input) {
        NSString *nodeID = item[@"id"];
        NSString *role = nodes[nodeID][@"role"];
        if (![role isEqual:@"AXWindow"] && ![role isEqual:@"AXSheet"]) continue;
        id children = nodes[nodeID][@"attributes"][@"AXChildren"];
        if (![children isKindOfClass:NSArray.class]) continue;
        for (id reference in children) {
            if (![reference isKindOfClass:NSDictionary.class] || ![reference[@"$ax"] isEqual:@"element"]) continue;
            NSString *child = reference[@"value"];
            NSString *childRole = nodes[child][@"role"];
            if (![displayRoots containsObject:child] ||
                (![childRole isEqual:@"AXWindow"] && ![childRole isEqual:@"AXSheet"])) continue;
            NSString *ancestor = nodeID;
            NSUInteger depth = 0;
            while (ancestor && ![ancestor isEqual:child] && depth++ < 128) ancestor = parents[ancestor];
            if (ancestor || depth >= 128) continue;
            parents[child] = nodeID;
            nodes[nodeID][@"children"] = [nodes[nodeID][@"children"] arrayByAddingObject:child];
            [displayRoots removeObject:child];
        }
    }
    NSMutableDictionary *immutable = [NSMutableDictionary dictionaryWithCapacity:nodes.count];
    for (NSString *nodeID in nodes) {
        NSMutableDictionary *node = nodes[nodeID];
        NSString *declaredParent = node[@"parent"], *parent = parents[nodeID];
        if (declaredParent && ![declaredParent isEqual:parent]) return nil;
        if (parent) node[@"parent"] = parent;
        immutable[nodeID] = [node copy];
    }
    NSString *focused = message[@"focused"];
    if (focused && (!VmAccessibilityIsIdentifier(focused) || !nodes[focused])) return nil;
    if (!focused) {
        for (NSArray *entry in queue) if ([nodes[entry[0]][@"focused"] boolValue]) { focused = entry[0]; break; }
    }
    VmAccessibilitySnapshot *snapshot = [VmAccessibilitySnapshot new];
    snapshot.message = message;
    snapshot.session = message[@"session"];
    snapshot.revision = [message[@"revision"] unsignedLongLongValue];
    snapshot.app = @{ @"pid": app[@"pid"], @"name": app[@"name"], @"bundleId": app[@"bundleId"] };
    snapshot.display = NSMakeRect([display[@"x"] doubleValue], [display[@"y"] doubleValue],
                                  [display[@"width"] doubleValue], [display[@"height"] doubleValue]);
    snapshot.pixels = NSMakeSize([display[@"pixelWidth"] doubleValue], [display[@"pixelHeight"] doubleValue]);
    snapshot.nodes = immutable;
    snapshot.roots = displayRoots;
    snapshot.focused = focused;
    snapshot.truncated = [message[@"truncated"] boolValue];
    snapshot.refreshGeneration = [message[@"refreshGeneration"] unsignedLongLongValue];
    return snapshot;
}

NSValue *VmAccessibilityTransformGeometry(NSValue *value, VmAccessibilitySnapshot *snapshot, NSRect frame, BOOL toGuest) {
    if (!snapshot || snapshot.pixels.width <= 0 || snapshot.pixels.height <= 0 || NSIsEmptyRect(frame)) return nil;
    if (!strcmp(value.objCType, @encode(NSRange))) return value;
    CGFloat scale = MIN(frame.size.width / snapshot.pixels.width, frame.size.height / snapshot.pixels.height);
    NSSize content = NSMakeSize(snapshot.pixels.width * scale, snapshot.pixels.height * scale);
    CGFloat sx = content.width / snapshot.display.size.width, sy = content.height / snapshot.display.size.height;
    CGFloat left = frame.origin.x + (frame.size.width - content.width) / 2;
    CGFloat top = frame.origin.y + (frame.size.height + content.height) / 2;
    NSPoint (^point)(NSPoint) = ^NSPoint(NSPoint p) {
        return toGuest ? NSMakePoint((p.x - left) / sx + NSMinX(snapshot.display), (top - p.y) / sy + NSMinY(snapshot.display)) :
            NSMakePoint(left + (p.x - NSMinX(snapshot.display)) * sx, top - (p.y - NSMinY(snapshot.display)) * sy);
    };
    NSSize (^size)(NSSize) = ^NSSize(NSSize s) { return toGuest ? NSMakeSize(s.width / sx, s.height / sy) : NSMakeSize(s.width * sx, s.height * sy); };
    if (!strcmp(value.objCType, @encode(NSPoint))) return [NSValue valueWithPoint:point(value.pointValue)];
    if (!strcmp(value.objCType, @encode(NSSize))) return [NSValue valueWithSize:size(value.sizeValue)];
    if (!strcmp(value.objCType, @encode(NSRect))) {
        NSRect r = value.rectValue;
        NSPoint origin = point(NSMakePoint(NSMinX(r), NSMaxY(r)));
        return [NSValue valueWithRect:(NSRect){origin, size(r.size)}];
    }
    return nil;
}

BOOL VmAccessibilityDisplayPixelsCompatible(NSSize output, NSSize rendered) {
    if (output.width <= 0 || output.height <= 0) return YES;
    if (rendered.width <= 0 || rendered.height <= 0) return NO;
    CGFloat scale = MIN(output.width / rendered.width, output.height / rendered.height);
    return fabs(output.width - rendered.width * scale) <= 1 &&
        fabs(output.height - rendered.height * scale) <= 1;
}
