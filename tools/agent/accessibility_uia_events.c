#define WIN32_LEAN_AND_MEAN
#include "accessibility_uia_events.h"
/* SDK module constants have external linkage in C. accessibility_uia.c
 * includes their definitions once; this TU imports only the values it uses. */
#define __UIA_PatternIds_MODULE_DEFINED__
#define __UIA_EventIds_MODULE_DEFINED__
#define __UIA_PropertyIds_MODULE_DEFINED__
#define __UIA_TextAttributeIds_MODULE_DEFINED__
#define __UIA_ControlTypeIds_MODULE_DEFINED__
#define __UIA_AnnotationTypes_MODULE_DEFINED__
#define __UIA_StyleIds_MODULE_DEFINED__
#define __UIA_LandmarkTypeIds_MODULE_DEFINED__
#define __UIA_HeadingLevelIds_MODULE_DEFINED__
#define __UIA_ChangeIds_MODULE_DEFINED__
#define __UIA_MetadataIds_MODULE_DEFINED__
/* UIAutomationCoreApi.h defines this one outside the module guards. It is
 * unused here; give this TU's SDK definition a private namespace. */
#define UIA_ScrollPatternNoScroll asb_events_sdk_scroll_no_scroll
#include <UIAutomationClient.h>
#undef UIA_ScrollPatternNoScroll
extern const long UIA_ExpandCollapseExpandCollapseStatePropertyId;
extern const long UIA_GridColumnCountPropertyId;
extern const long UIA_GridRowCountPropertyId;
extern const long UIA_HasKeyboardFocusPropertyId;
extern const long UIA_IsEnabledPropertyId;
extern const long UIA_MenuClosedEventId;
extern const long UIA_MenuOpenedEventId;
extern const long UIA_NamePropertyId;
extern const long UIA_ProcessIdPropertyId;
extern const long UIA_RangeValueIsReadOnlyPropertyId;
extern const long UIA_RangeValueMaximumPropertyId;
extern const long UIA_RangeValueMinimumPropertyId;
extern const long UIA_RangeValueValuePropertyId;
extern const long UIA_ScrollHorizontalScrollPercentPropertyId;
extern const long UIA_ScrollVerticalScrollPercentPropertyId;
extern const long UIA_SelectionItemIsSelectedPropertyId;
extern const long UIA_SelectionItem_ElementAddedToSelectionEventId;
extern const long UIA_SelectionItem_ElementRemovedFromSelectionEventId;
extern const long UIA_SelectionItem_ElementSelectedEventId;
extern const long UIA_Selection_InvalidatedEventId;
extern const long UIA_Text_TextChangedEventId;
extern const long UIA_Text_TextSelectionChangedEventId;
extern const long UIA_ToggleToggleStatePropertyId;
extern const long UIA_ValueIsReadOnlyPropertyId;
extern const long UIA_ValueValuePropertyId;
extern const long UIA_WindowWindowVisualStatePropertyId;
extern const long UIA_Window_WindowClosedEventId;
extern const long UIA_Window_WindowOpenedEventId;
#include <stddef.h>
#include <stdlib.h>

typedef struct Changes {
    volatile LONG refs;
    volatile LONG64 generation;
    volatile LONG64 tick;
} Changes;
typedef struct Sink {
    IUIAutomationEventHandler event;
    IUIAutomationPropertyChangedEventHandler property;
    IUIAutomationStructureChangedEventHandler structure;
    IUIAutomationFocusChangedEventHandler focus;
    volatile LONG refs;
    volatile LONG enabled;
    int process;
    Changes *changes;
    IUnknown *marshaler;
} Sink;
struct AccessibilityUiaEvents {
    IUIAutomation *automation;
    IUIAutomation6 *automation6;
    IUIAutomationElement *root;
    IUIAutomationEventHandlerGroup *group;
    Sink *sink;
    Changes *changes;
    BOOL group_registered, focus_registered;
};
static void changes_release(Changes *c) { if (!InterlockedDecrement(&c->refs)) free(c); }
static ULONG sink_addref(Sink *s) { return (ULONG)InterlockedIncrement(&s->refs); }
static ULONG sink_release(Sink *s) {
    ULONG refs = (ULONG)InterlockedDecrement(&s->refs);
    if (!refs) {
        if (s->marshaler) s->marshaler->lpVtbl->Release(s->marshaler);
        changes_release(s->changes);
        free(s);
    }
    return refs;
}
static HRESULT sink_query(Sink *s, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IUIAutomationEventHandler)) *out = &s->event;
    else if (IsEqualIID(iid, &IID_IUIAutomationPropertyChangedEventHandler)) *out = &s->property;
    else if (IsEqualIID(iid, &IID_IUIAutomationStructureChangedEventHandler)) *out = &s->structure;
    else if (IsEqualIID(iid, &IID_IUIAutomationFocusChangedEventHandler)) *out = &s->focus;
    else if (IsEqualIID(iid, &IID_IMarshal) && s->marshaler)
        return s->marshaler->lpVtbl->QueryInterface(s->marshaler, iid, out);
    else return E_NOINTERFACE;
    sink_addref(s);
    return S_OK;
}
/* UIA can retain callbacks after unsubscription. Each sink owns the shared
 * counters and has its own disable gate, never a pointer to the collector. */
static void signal_change(Sink *s) {
    LONG64 now, previous;
    if (!InterlockedCompareExchange(&s->enabled, 0, 0)) return;
    now = (LONG64)GetTickCount64();
    previous = InterlockedCompareExchange64(&s->changes->tick, 0, 0);
    while (previous < now) {
        LONG64 actual = InterlockedCompareExchange64(&s->changes->tick, now, previous);
        if (actual == previous) break;
        previous = actual;
    }
    InterlockedIncrement64(&s->changes->generation);
}
#define SINK_FROM(p, member) ((Sink *)((char *)(p) - offsetof(Sink, member)))
#define UNKNOWN_METHODS(prefix, type, member) \
static HRESULT STDMETHODCALLTYPE prefix##_query(type *p, REFIID iid, void **out) { return sink_query(SINK_FROM(p, member), iid, out); } \
static ULONG STDMETHODCALLTYPE prefix##_addref(type *p) { return sink_addref(SINK_FROM(p, member)); } \
static ULONG STDMETHODCALLTYPE prefix##_release(type *p) { return sink_release(SINK_FROM(p, member)); }
UNKNOWN_METHODS(event, IUIAutomationEventHandler, event)
UNKNOWN_METHODS(property, IUIAutomationPropertyChangedEventHandler, property)
UNKNOWN_METHODS(structure, IUIAutomationStructureChangedEventHandler, structure)
UNKNOWN_METHODS(focus, IUIAutomationFocusChangedEventHandler, focus)
static HRESULT STDMETHODCALLTYPE event_handle(IUIAutomationEventHandler *p, IUIAutomationElement *sender, EVENTID id) {
    (void)sender; (void)id; signal_change(SINK_FROM(p, event)); return S_OK;
}
static HRESULT STDMETHODCALLTYPE property_handle(IUIAutomationPropertyChangedEventHandler *p, IUIAutomationElement *sender, PROPERTYID id, VARIANT value) {
    (void)sender; (void)id; (void)value; signal_change(SINK_FROM(p, property)); return S_OK;
}
static HRESULT STDMETHODCALLTYPE structure_handle(IUIAutomationStructureChangedEventHandler *p, IUIAutomationElement *sender, enum StructureChangeType type, SAFEARRAY *ids) {
    (void)sender; (void)type; (void)ids; signal_change(SINK_FROM(p, structure)); return S_OK;
}
static HRESULT STDMETHODCALLTYPE focus_handle(IUIAutomationFocusChangedEventHandler *p, IUIAutomationElement *sender) {
    Sink *s = SINK_FROM(p, focus);
    int process = 0;
    /* Only registration-cached PID: never read live properties or walk trees. */
    if (InterlockedCompareExchange(&s->enabled, 0, 0) && sender &&
        SUCCEEDED(sender->lpVtbl->get_CachedProcessId(sender, &process)) && process == s->process)
        signal_change(s);
    return S_OK;
}
static IUIAutomationEventHandlerVtbl event_vtbl = {event_query, event_addref, event_release, event_handle};
static IUIAutomationPropertyChangedEventHandlerVtbl property_vtbl = {property_query, property_addref, property_release, property_handle};
static IUIAutomationStructureChangedEventHandlerVtbl structure_vtbl = {structure_query, structure_addref, structure_release, structure_handle};
static IUIAutomationFocusChangedEventHandlerVtbl focus_vtbl = {focus_query, focus_addref, focus_release, focus_handle};
static HRESULT sink_create(Changes *changes, int process, Sink **out) {
    Sink *s = calloc(1, sizeof(*s));
    HRESULT hr;
    *out = NULL;
    if (!s) return E_OUTOFMEMORY;
    s->event.lpVtbl = &event_vtbl; s->property.lpVtbl = &property_vtbl;
    s->structure.lpVtbl = &structure_vtbl; s->focus.lpVtbl = &focus_vtbl;
    s->refs = 1; s->enabled = 1; s->process = process; s->changes = changes;
    InterlockedIncrement(&changes->refs);
    hr = CoCreateFreeThreadedMarshaler((IUnknown *)&s->event, &s->marshaler);
    if (FAILED(hr)) { sink_release(s); return hr; }
    *out = s; return S_OK;
}
HRESULT accessibility_uia_events_create(IUIAutomation *automation, AccessibilityUiaEvents **out) {
    AccessibilityUiaEvents *e;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!automation) return E_POINTER;
    e = calloc(1, sizeof(*e));
    if (!e) return E_OUTOFMEMORY;
    e->changes = calloc(1, sizeof(*e->changes));
    if (!e->changes) { free(e); return E_OUTOFMEMORY; }
    e->changes->refs = 1;
    hr = automation->lpVtbl->QueryInterface(automation, &IID_IUIAutomation6, (void **)&e->automation6);
    if (FAILED(hr)) { changes_release(e->changes); free(e); return hr; }
    e->automation = automation; automation->lpVtbl->AddRef(automation);
    *out = e; return S_OK;
}
void accessibility_uia_events_reset(AccessibilityUiaEvents *e) {
    if (!e) return;
    if (e->sink) InterlockedExchange(&e->sink->enabled, 0);
    if (e->focus_registered) e->automation->lpVtbl->RemoveFocusChangedEventHandler(e->automation, &e->sink->focus);
    if (e->group_registered) e->automation6->lpVtbl->RemoveEventHandlerGroup(e->automation6, e->root, e->group);
    e->focus_registered = e->group_registered = FALSE;
    if (e->group) e->group->lpVtbl->Release(e->group);
    if (e->root) e->root->lpVtbl->Release(e->root);
    if (e->sink) sink_release(e->sink);
    e->group = NULL; e->root = NULL; e->sink = NULL;
}
void accessibility_uia_events_destroy(AccessibilityUiaEvents *e) {
    if (!e) return;
    accessibility_uia_events_reset(e);
    e->automation6->lpVtbl->Release(e->automation6);
    e->automation->lpVtbl->Release(e->automation);
    changes_release(e->changes); free(e);
}
uint64_t accessibility_uia_events_generation(AccessibilityUiaEvents *e) {
    return e ? (uint64_t)InterlockedCompareExchange64(&e->changes->generation, 0, 0) : 0;
}
uint64_t accessibility_uia_events_last_change_time(AccessibilityUiaEvents *e) {
    return e ? (uint64_t)InterlockedCompareExchange64(&e->changes->tick, 0, 0) : 0;
}
HRESULT accessibility_uia_events_observe(AccessibilityUiaEvents *e, IUIAutomationElement *root) {
    PROPERTYID properties[] = {
        UIA_NamePropertyId, UIA_IsEnabledPropertyId, UIA_HasKeyboardFocusPropertyId,
        UIA_ValueValuePropertyId, UIA_ValueIsReadOnlyPropertyId,
        UIA_RangeValueValuePropertyId, UIA_RangeValueMinimumPropertyId, UIA_RangeValueMaximumPropertyId,
        UIA_RangeValueIsReadOnlyPropertyId, UIA_ToggleToggleStatePropertyId,
        UIA_SelectionItemIsSelectedPropertyId, UIA_ExpandCollapseExpandCollapseStatePropertyId,
        UIA_GridRowCountPropertyId, UIA_GridColumnCountPropertyId,
        UIA_ScrollHorizontalScrollPercentPropertyId, UIA_ScrollVerticalScrollPercentPropertyId,
        UIA_WindowWindowVisualStatePropertyId
    };
    EVENTID events[] = { UIA_Text_TextChangedEventId, UIA_Text_TextSelectionChangedEventId,
        UIA_SelectionItem_ElementSelectedEventId, UIA_SelectionItem_ElementAddedToSelectionEventId,
        UIA_SelectionItem_ElementRemovedFromSelectionEventId, UIA_Selection_InvalidatedEventId,
        UIA_Window_WindowOpenedEventId, UIA_Window_WindowClosedEventId, UIA_MenuOpenedEventId, UIA_MenuClosedEventId };
    IUIAutomationCacheRequest *cache = NULL;
    HRESULT hr;
    int process = 0;
    size_t i;
    if (!e) return E_POINTER;
    accessibility_uia_events_reset(e);
    if (!root) return S_OK;
#define CHECK(call) do { hr = (call); if (FAILED(hr)) goto failed; } while (0)
    CHECK(root->lpVtbl->get_CurrentProcessId(root, &process));
    e->root = root; root->lpVtbl->AddRef(root);
    CHECK(sink_create(e->changes, process, &e->sink));
    CHECK(e->automation6->lpVtbl->CreateEventHandlerGroup(e->automation6, &e->group));
    /* Semantic changes only; omit continuously changing geometry. */
    CHECK(e->group->lpVtbl->AddPropertyChangedEventHandler(e->group, TreeScope_Subtree, NULL,
        &e->sink->property, properties, (int)ARRAYSIZE(properties)));
    CHECK(e->group->lpVtbl->AddStructureChangedEventHandler(e->group, TreeScope_Subtree, NULL, &e->sink->structure));
    for (i = 0; i < ARRAYSIZE(events); ++i)
        CHECK(e->group->lpVtbl->AddAutomationEventHandler(e->group, events[i], TreeScope_Subtree, NULL, &e->sink->event));
    CHECK(e->automation6->lpVtbl->AddEventHandlerGroup(e->automation6, root, e->group));
    e->group_registered = TRUE;
    CHECK(e->automation->lpVtbl->CreateCacheRequest(e->automation, &cache));
    CHECK(cache->lpVtbl->put_TreeScope(cache, TreeScope_Element));
    CHECK(cache->lpVtbl->put_AutomationElementMode(cache, AutomationElementMode_None));
    CHECK(cache->lpVtbl->AddProperty(cache, UIA_ProcessIdPropertyId));
    CHECK(e->automation->lpVtbl->AddFocusChangedEventHandler(e->automation, cache, &e->sink->focus));
    e->focus_registered = TRUE;
    cache->lpVtbl->Release(cache);
    return S_OK;
failed:
    if (cache) cache->lpVtbl->Release(cache);
    accessibility_uia_events_reset(e);
    return hr;
#undef CHECK
}
