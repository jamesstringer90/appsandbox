/*
 * vz_display -- NSWindowController hosting a VZVirtualMachineView.
 */

#import <Cocoa/Cocoa.h>
#import <Virtualization/Virtualization.h>

@class VzVm;

@interface VzDisplayWindow : NSWindowController

/* Set in windowWillClose: so callers can tell a window the user closed (X) from a
   live one without touching AppKit off the main thread. The headless daemon reads
   it to report displayOpen and to reap a self-closed window before reopening. */
@property (nonatomic) BOOL userClosed;
@property (nonatomic, readonly) uint64_t vmId;
@property (nonatomic, strong, readonly) VZVirtualMachineView *vmView;

- (instancetype)initWithVzVm:(VzVm *)vm vmId:(uint64_t)vmId;
- (instancetype)initWithVzVm:(VzVm *)vm vmId:(uint64_t)vmId offscreen:(BOOL)offscreen;
- (void)showDisplay;
- (void)prepareForAgentInput;
- (void)finishPreparingForAgentInput;

@end
