#ifndef ASB_MONITOR_EDID_H
#define ASB_MONITOR_EDID_H

#include <windows.h>
#include "edid.h"

typedef void (*MonitorVisitor)(const wchar_t *path, const wchar_t *name, void *context);
void enum_monitor_edid_sources(MonitorVisitor visit, void *context);
BOOL read_monitor_edid(const wchar_t *path, AsbEdid *edid);

#endif
