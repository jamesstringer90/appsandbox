/*
 * vz_display -- NSWindowController hosting a VZVirtualMachineView.
 */

#import <Cocoa/Cocoa.h>
#import <Virtualization/Virtualization.h>

@class VzVm;

@interface VzDisplayWindow : NSWindowController

- (instancetype)initWithVzVm:(VzVm *)vm;
- (void)showDisplay;

@end
