#ifndef ASB_ACCESSIBILITY_UIA_EVENTS_H
#define ASB_ACCESSIBILITY_UIA_EVENTS_H
#include <windows.h>
#include <ole2.h>
typedef struct IUIAutomation IUIAutomation;
typedef struct IUIAutomationElement IUIAutomationElement;
#include <stdint.h>
typedef struct AccessibilityUiaEvents AccessibilityUiaEvents;
HRESULT accessibility_uia_events_create(IUIAutomation *, AccessibilityUiaEvents **);
void accessibility_uia_events_destroy(AccessibilityUiaEvents *);
HRESULT accessibility_uia_events_observe(AccessibilityUiaEvents *, IUIAutomationElement *);
void accessibility_uia_events_reset(AccessibilityUiaEvents *);
uint64_t accessibility_uia_events_generation(AccessibilityUiaEvents *);
uint64_t accessibility_uia_events_last_change_time(AccessibilityUiaEvents *);
#endif
