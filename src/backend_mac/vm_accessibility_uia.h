#import "vm_accessibility_snapshot.h"

@interface VmAccessibilityUia : NSObject
- (VmAccessibilitySnapshot *)snapshot:(NSDictionary *)message;
- (NSDictionary *)request:(NSDictionary *)message send:(NSDictionary *(^)(NSDictionary *))send;
@end
