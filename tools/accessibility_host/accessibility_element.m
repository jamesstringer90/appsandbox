#import "accessibility_element.h"
#import "../../src/backend_mac/vm_accessibility_snapshot.h"
#import "../transport/accessibility_attributes_mac.h"
#import "../../tools/transport/accessibility_value_mac.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@implementation VmAccessibilityElement
- (NSDictionary *)record { return [self.owner recordForElement:self]; }
- (void)retire {
    if (_retired) return;
    _retired = YES;
    NSAccessibilityPostNotification(self, NSAccessibilityUIElementDestroyedNotification);
}
- (void)dealloc {
    if (!_retired) NSAccessibilityPostNotification(self, NSAccessibilityUIElementDestroyedNotification);
}
- (BOOL)isAccessibilityElement { return !self.retired && [self.owner isCurrentElement:self]; }
- (BOOL)accessibilityIsIgnored { return !self.isAccessibilityElement; }
- (BOOL)accessibilityNotifiesWhenDestroyed { return YES; }
- (NSString *)accessibilityRole {
    NSString *role = self.record[@"role"];
    if (self.container || [role isEqual:@"AXWindow"] || [role isEqual:@"AXSheet"] ||
        [role isEqual:@"AXApplication"]) return NSAccessibilityGroupRole;
    return role ?: NSAccessibilityStaticTextRole;
}
- (id)accessibilityValue {
    if (self.record[@"attributeErrors"][@"AXValue"]) return nil;
    id encoded = self.record[@"attributes"][@"AXValue"];
    if ([self.record[@"secure"] boolValue]) {
        id value = encoded ?: self.record[@"value"];
        return AsbAXIsMaskedValue(value) ? value : nil;
    }
    id value = encoded ? [self decode:encoded geometry:YES] : self.record[@"value"];
    return value == NSNull.null ? nil : value;
}
- (id)accessibilityParent { return [self.owner parentForElement:self]; }
- (id)accessibilityWindow { return self.owner.window; }
- (id)accessibilityTopLevelUIElement { return self.owner.window; }
- (NSArray *)accessibilityChildren {
    NSDictionary *record = self.record;
    if (record[@"attributeErrors"][@"AXChildren"]) return nil;
    if (!self.isAccessibilityElement) return @[];
    id encoded = record[@"attributes"][@"AXChildren"];
    if (encoded) {
        id children = [self decode:encoded geometry:NO];
        return [children isKindOfClass:NSArray.class] ? children : nil;
    }
    return [self.owner childrenForElement:self];
}
- (NSRect)accessibilityFrame { return [self.owner frameForElement:self]; }
- (id)accessibilityFocusedUIElement { return [self.owner focusedGuestElement]; }
- (id)hitTestGuest:(NSPoint)point {
    if (!self.isAccessibilityElement) return nil;
    for (VmAccessibilityElement *child in [self.owner childrenForElement:self]) {
        id hit = [child hitTestGuest:point];
        if (hit) return hit;
    }
    return NSPointInRect(point, self.accessibilityFrame) ? self : nil;
}
- (id)accessibilityHitTest:(NSPoint)point { return [self hitTestGuest:point]; }
- (id)decode:(id)value geometry:(BOOL)geometry {
    return AsbAXDecodeValue(value, NO, ^id(NSString *identifier) {
        return [self.owner elementForIdentifier:identifier session:self.session];
    }, geometry ? ^NSValue *(NSValue *v) { return [self.owner transformGeometry:v toGuest:NO]; } : nil, 0);
}
- (id)encode:(id)value geometry:(BOOL)geometry {
    id wire = AsbAXEncodeValue(value, ^id(id candidate) {
        if (![candidate isKindOfClass:VmAccessibilityElement.class]) return nil;
        VmAccessibilityElement *element = candidate;
        if (element.owner != self.owner || ![element.session isEqual:self.session]) return NSNull.null;
        return @{@"$ax": @"element", @"value": element.nodeID};
    }, 0);
    if (!geometry || !wire) return wire;
    id converted = AsbAXDecodeValue(wire, NO, ^id(NSString *identifier) {
        return [self.owner elementForIdentifier:identifier session:self.session];
    }, ^NSValue *(NSValue *v) { return [self.owner transformGeometry:v toGuest:YES]; }, 0);
    return converted ? [self encode:converted geometry:NO] : nil;
}
- (NSArray *)accessibilityAttributeNames {
    if (!self.isAccessibilityElement) return @[];
    NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSetWithArray:self.record[@"attributeNames"] ?: @[]];
    if (!self.record[@"attributeNames"] || self.container || self.status)
        [names addObjectsFromArray:@[@"AXRole", @"AXRoleDescription", @"AXParent", @"AXWindow", @"AXTopLevelUIElement",
            @"AXChildren", @"AXPosition", @"AXSize", @"AXEnabled", @"AXFocused"]];
    NSDictionary *keys = @{@"title": @"AXTitle", @"label": @"AXDescription", @"value": @"AXValue", @"subrole": @"AXSubrole", @"selectedTextRange": @"AXSelectedTextRange"};
    for (NSString *key in keys) if (self.record[key]) [names addObject:keys[key]];
    [names addObjectsFromArray:[self.record[@"attributes"] allKeys] ?: @[]];
    if ([self.record[@"secure"] boolValue]) {
        for (NSString *name in [names.array copy])
            if ([name containsString:@"Text"] || [name containsString:@"Character"] ||
                ([name isEqual:@"AXValue"] && !self.accessibilityValue)) [names removeObject:name];
    }
    return names.array;
}
- (id)accessibilityAttributeValue:(NSString *)attribute {
    if (!self.isAccessibilityElement) return nil;
    if (self.record[@"attributeErrors"][attribute]) return nil;
    if ([attribute isEqual:@"AXRole"]) return self.accessibilityRole;
    if ([attribute isEqual:@"AXParent"]) return self.accessibilityParent;
    if ([attribute isEqual:@"AXWindow"] || [attribute isEqual:@"AXTopLevelUIElement"]) return self.owner.window;
    if ([attribute isEqual:@"AXChildren"]) return self.accessibilityChildren;
    if ([attribute isEqual:@"AXPosition"]) return [NSValue valueWithPoint:self.accessibilityFrame.origin];
    if ([attribute isEqual:@"AXSize"]) return [NSValue valueWithSize:self.accessibilityFrame.size];
    if ([attribute isEqual:@"AXFrame"]) return [NSValue valueWithRect:self.accessibilityFrame];
    if ([attribute isEqual:@"AXFocused"]) return @([self.owner focusedGuestElement] == self);
    if ([attribute isEqual:@"AXEnabled"]) return @(!self.status && [self.record[@"enabled"] boolValue]);
    if ([attribute isEqual:@"AXTitle"]) return self.record[@"title"];
    if ([attribute isEqual:@"AXDescription"]) return self.record[@"label"];
    if ([attribute isEqual:@"AXValue"]) return self.accessibilityValue;
    if ([attribute isEqual:@"AXSubrole"]) return self.container ? nil : self.record[@"subrole"];
    if ([self.record[@"secure"] boolValue] && ([attribute containsString:@"Text"] || [attribute containsString:@"Character"])) return nil;
    if (self.record[@"attributes"][attribute]) {
        id result = [self decode:self.record[@"attributes"][attribute] geometry:YES];
        return result == NSNull.null ? nil : result;
    }
    if ([attribute isEqual:@"AXRoleDescription"]) return NSAccessibilityRoleDescription(self.accessibilityRole, self.record[@"subrole"]);
    if ([attribute isEqual:@"AXSelectedTextRange"] && self.record[@"selectedTextRange"]) {
        NSArray *range = self.record[@"selectedTextRange"];
        return [NSValue valueWithRange:NSMakeRange([range[0] unsignedIntegerValue], [range[1] unsignedIntegerValue])];
    }
    if (AsbAXAttributeRequiresLiveQuery(attribute) && [self.record[@"attributeNames"] containsObject:attribute]) {
        NSDictionary *response = [self.owner request:@{@"action": @"getAttribute", @"attribute": attribute} element:self];
        id result = [response[@"ok"] boolValue] ? [self decode:response[@"value"] geometry:YES] : nil;
        return result == NSNull.null ? nil : result;
    }
    return nil;
}
- (BOOL)accessibilityIsAttributeSettable:(NSString *)attribute {
    if (!self.isAccessibilityElement || self.container || self.status) return NO;
    if (self.record[@"settable"][attribute]) return [self.record[@"settable"][attribute] boolValue];
    NSArray *writable = self.record[@"writableAttributes"];
    if (writable) return [writable containsObject:attribute];
    if ([attribute isEqual:@"AXValue"]) return [self.record[@"writableValue"] boolValue];
    if ([attribute isEqual:@"AXFocused"]) return [self.record[@"writableFocused"] boolValue];
    if ([attribute isEqual:@"AXSelectedTextRange"]) return [self.record[@"writableSelectedTextRange"] boolValue];
    return NO;
}
- (void)accessibilitySetValue:(id)value forAttribute:(NSString *)attribute {
    if (![self accessibilityIsAttributeSettable:attribute]) {
        NSAccessibilityRaiseBadArgumentException(self, attribute, value);
        return;
    }
    id wire = nil;
    if ([attribute isEqual:@"AXPosition"] && [value isKindOfClass:NSValue.class] && !strcmp([value objCType], @encode(NSPoint))) {
        NSRect frame = self.accessibilityFrame;
        frame.origin = [value pointValue];
        NSRect guest = [[self.owner transformGeometry:[NSValue valueWithRect:frame] toGuest:YES] rectValue];
        wire = AsbAXEncodeValue([NSValue valueWithPoint:guest.origin], nil, 0);
    } else wire = [self encode:value geometry:YES];
    NSDictionary *response = wire ? [self.owner request:@{@"action": @"setAttribute", @"attribute": attribute, @"value": wire} element:self] : nil;
    if (![response[@"ok"] boolValue]) NSAccessibilityRaiseBadArgumentException(self, attribute, value);
}
- (NSArray *)accessibilityActionNames {
    if (self.record[@"actionNamesError"]) return nil;
    return self.isAccessibilityElement ? (self.record[@"actions"] ?: @[]) : @[];
}
- (NSString *)accessibilityActionDescription:(NSString *)action {
    return self.record[@"actionDescriptions"][action] ?: NSAccessibilityActionDescription(action);
}
- (void)accessibilityPerformAction:(NSString *)action {
    if (![[self accessibilityActionNames] containsObject:action] ||
        ![[self.owner request:@{@"action": action} element:self][@"ok"] boolValue])
        NSAccessibilityRaiseBadArgumentException(self, action, nil);
}
- (NSArray *)accessibilityParameterizedAttributeNames {
    return self.isAccessibilityElement && ![self.record[@"secure"] boolValue] ? (self.record[@"parameterizedNames"] ?: @[]) : @[];
}
- (id)accessibilityAttributeValue:(NSString *)attribute forParameter:(id)parameter {
    if (![[self accessibilityParameterizedAttributeNames] containsObject:attribute]) return nil;
    BOOL inputGeometry = ![attribute isEqual:@"AXScreenPointForLayoutPoint"] && ![attribute isEqual:@"AXScreenSizeForLayoutSize"];
    BOOL outputGeometry = ![attribute isEqual:@"AXLayoutPointForScreenPoint"] && ![attribute isEqual:@"AXLayoutSizeForScreenSize"];
    id wire = [self encode:parameter geometry:inputGeometry];
    NSDictionary *response = wire ? [self.owner request:@{@"action": @"getParameterizedAttribute", @"attribute": attribute, @"value": wire} element:self] : nil;
    id result = [response[@"ok"] boolValue] ? [self decode:response[@"value"] geometry:outputGeometry] : nil;
    return result == NSNull.null ? nil : result;
}
- (NSUInteger)accessibilityArrayAttributeCount:(NSString *)attribute {
    id value = [self accessibilityAttributeValue:attribute];
    return [value isKindOfClass:NSArray.class] ? [value count] : 0;
}
- (NSArray *)accessibilityArrayAttributeValues:(NSString *)attribute index:(NSUInteger)index maxCount:(NSUInteger)maximum {
    id value = [self accessibilityAttributeValue:attribute];
    if (![value isKindOfClass:NSArray.class]) return nil;
    if (index >= [value count]) return @[];
    return [value subarrayWithRange:NSMakeRange(index, MIN(maximum, [value count] - index))];
}
- (NSUInteger)accessibilityIndexOfChild:(id)child { return [self.accessibilityChildren indexOfObjectIdenticalTo:child]; }
@end

#pragma clang diagnostic pop
