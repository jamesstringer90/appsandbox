#import <Cocoa/Cocoa.h>
#include <math.h>

static inline NSTimeInterval VmAccessibilityNow(void) {
    return NSProcessInfo.processInfo.systemUptime;
}

static inline BOOL VmAccessibilityIsString(id value, NSUInteger maximum) {
    return [value isKindOfClass:NSString.class] && [value length] <= maximum;
}

static inline BOOL VmAccessibilityIsNumber(id value, double minimum, double maximum) {
    return [value isKindOfClass:NSNumber.class] && isfinite([value doubleValue]) &&
        [value doubleValue] >= minimum && [value doubleValue] <= maximum;
}

static inline BOOL VmAccessibilityIsInteger(id value, double minimum, double maximum) {
    return VmAccessibilityIsNumber(value, minimum, maximum) && floor([value doubleValue]) == [value doubleValue];
}

static inline BOOL VmAccessibilityIsIdentifier(id value) {
    return VmAccessibilityIsString(value, 128) && [value length] > 0;
}

static inline BOOL VmAccessibilityIsSessionIdentifier(id value) {
    return VmAccessibilityIsIdentifier(value) && [[NSUUID alloc] initWithUUIDString:value] != nil;
}

@interface VmAccessibilitySnapshot : NSObject
@property NSDictionary *message;
@property (copy) NSString *session;
@property uint64_t revision;
@property (copy) NSDictionary *app;
@property NSRect display;
@property NSSize pixels;
@property (copy) NSDictionary<NSString *, NSDictionary *> *nodes;
@property (copy) NSArray<NSString *> *roots;
@property (copy) NSString *focused;
@property BOOL truncated;
@property uint64_t refreshGeneration;
@end

NSDictionary *VmAccessibilityParseRecord(NSDictionary *source);
VmAccessibilitySnapshot *VmAccessibilityParseSnapshot(NSDictionary *message);
NSValue *VmAccessibilityTransformGeometry(NSValue *value, VmAccessibilitySnapshot *snapshot, NSRect frame, BOOL toGuest);
BOOL VmAccessibilityDisplayPixelsCompatible(NSSize output, NSSize rendered);
