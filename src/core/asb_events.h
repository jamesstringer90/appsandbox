#ifndef ASB_EVENTS_H
#define ASB_EVENTS_H

#include "asb_types.h"

#ifdef _WIN32
#ifdef ASB_BUILDING_DLL
#define ASB_EVENTS_API __declspec(dllexport)
#else
#define ASB_EVENTS_API __declspec(dllimport)
#endif
#else
#define ASB_EVENTS_API
#endif

ASB_EVENTS_API unsigned asb_events_subscribe(CoreVmEventCallback callback, void *context);
ASB_EVENTS_API void asb_events_unsubscribe(unsigned subscription);

ASB_EVENTS_API void asb_events_emit(const CoreVmEvent *event);
ASB_EVENTS_API void asb_events_changed(void);

#endif
