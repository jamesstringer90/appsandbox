#ifndef ASB_ACCESSIBILITY_UIA_H
#define ASB_ACCESSIBILITY_UIA_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include "accessibility_json.h"

typedef struct AccessibilityUia AccessibilityUia;
typedef int (*AccessibilityContinue)(void *context);

HRESULT accessibility_uia_create(AccessibilityUia **result);
void accessibility_uia_destroy(AccessibilityUia *uia);
void accessibility_uia_reset(AccessibilityUia *uia);
int accessibility_uia_active(const AccessibilityUia *uia);
int accessibility_uia_foreground_changed(const AccessibilityUia *uia);
uint64_t accessibility_uia_change_generation(const AccessibilityUia *uia);
uint64_t accessibility_uia_last_change_time(const AccessibilityUia *uia);

HRESULT accessibility_uia_capture(AccessibilityUia *uia, uint64_t deadline,
    AccessibilityContinue continuation, void *context, AccessibilityJson **snapshot);
AccessibilityJson *accessibility_uia_request(AccessibilityUia *uia,
    const AccessibilityJson *message, uint64_t deadline);

#endif
