#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "accessibility_uia.h"
#include "accessibility_uia_events.h"
#include "../transport/accessibility_protocol.h"
#include <ole2.h>
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <wtsapi32.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
#define TRY(call) do { hr = (call); if (FAILED(hr)) goto done; } while (0)
#define PATTERN(e, type, id, out) IUIAutomationElement_GetCurrentPatternAs((e), (id), &IID_##type, (void **)(out))
#define CACHED_PATTERN(e, type, id, out) IUIAutomationElement_GetCachedPatternAs((e), (id), &IID_##type, (void **)(out))
#define RECORD_BUCKETS 8192u
#define MAX_SNAPSHOT_TEXT (32u * 1024u * 1024u)

typedef AccessibilityJson Json;
typedef struct UiaRecord {
    IUIAutomationElement *element;
    BSTR identity, text, captured_text;
    wchar_t id[32];
    uint64_t number, identity_hash, capture, reference;
    DWORD pid;
    int secure, complete_text, captured_complete_text;
    Json *metadata, *captured_metadata;
    struct UiaRecord *next, *identity_next, *id_next;
} UiaRecord;

typedef struct UiaWork {
    size_t text_units;
    uint64_t deadline;
    HRESULT error;
    int truncated, capturing;
    AccessibilityContinue continuation;
    void *continuation_context;
} UiaWork;

struct AccessibilityUia {
    IUIAutomation *automation;
    IUIAutomation2 *timeouts;
    IUIAutomationCacheRequest *cache, *children_cache, *identity_cache;
    IUIAutomationTreeWalker *walker;
    AccessibilityUiaEvents *events;
    UiaRecord *records, **identities, **ids;
    size_t record_count;
    uint64_t next_id, capture;
    HWND foreground, event_foreground;
    DWORD session_id;
    UiaWork *work;
};

static int text_equal(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b) == 0; }
static Json *named(const Json *value, const wchar_t *key) { return ax_json_get(value, key); }
static const wchar_t *named_text(const Json *value, const wchar_t *key) { return ax_json_text(named(value, key)); }
static Json *typed(const wchar_t *type, Json *value) {
    Json *result = ax_json_object();
    ax_json_set(result, L"$uia", ax_json_string(type));
    ax_json_set(result, L"value", value);
    return result;
}
static Json *numbers(const double *values, size_t count) {
    Json *result = ax_json_array();
    size_t i;
    for (i = 0; i < count; ++i) ax_json_append(result, ax_json_number(values[i]));
    return result;
}
static Json *pair(double first, double second) {
    double values[] = {first, second};
    return numbers(values, 2);
}
static Json *range_value(size_t location, size_t length) { return pair((double)location, (double)length); }
static Json *element_reference(const UiaRecord *record) { return typed(L"element", ax_json_string(record->id)); }

static int cached_int(IUIAutomationElement *element, PROPERTYID property, int fallback) {
    VARIANT value;
    int result = fallback;
    VariantInit(&value);
    if (SUCCEEDED(IUIAutomationElement_GetCachedPropertyValue(element, property, &value))) {
        if (value.vt == VT_I4) result = value.lVal;
        else if (value.vt == VT_BOOL) result = value.boolVal != VARIANT_FALSE;
    }
    VariantClear(&value);
    return result;
}

static size_t limited_length(const wchar_t *text, size_t length, size_t limit) {
    if (length > limit) {
        length = limit;
        if (length && text[length - 1] >= 0xd800 && text[length - 1] <= 0xdbff) --length;
    }
    return length;
}
static Json *snapshot_string(AccessibilityUia *uia, const wchar_t *text, size_t length) {
    size_t available = MAX_SNAPSHOT_TEXT - uia->work->text_units;
    size_t count = limited_length(text, length, available < ASB_AX_MAX_STRING ? available : ASB_AX_MAX_STRING);
    if (count < length) uia->work->truncated = 1;
    uia->work->text_units += count;
    return ax_json_string_n(text, count);
}

static int is_masked_value(BSTR value) {
    size_t length = SysStringLen(value), i;
    if (length > ASB_AX_MAX_STRING) return 0;
    for (i = 0; i < length; ++i) {
        wchar_t character = value[i];
        if (character != L'\x2022' && character != L'\x25cf' &&
            character != L'\xf79a' && character != L'*') return 0;
    }
    return 1;
}
static int check_work(AccessibilityUia *uia) {
    if (FAILED(uia->work->error)) return 0;
    if (uia->work->deadline && GetTickCount64() >= uia->work->deadline) uia->work->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return SUCCEEDED(uia->work->error);
}
static int continue_capture(AccessibilityUia *uia) {
    if (!check_work(uia)) return 0;
    if (uia->work->continuation && !uia->work->continuation(uia->work->continuation_context) && check_work(uia))
        uia->work->error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return check_work(uia);
}
static int prepare(AccessibilityUia *uia, uint64_t deadline) {
    uint64_t now = GetTickCount64(), remaining;
    if (now >= deadline) return 0;
    remaining = deadline - now;
    if (uia->timeouts && FAILED(IUIAutomation2_put_TransactionTimeout(uia->timeouts, (DWORD)(remaining < 1000 ? remaining : 1000)))) return 0;
    return GetTickCount64() < deadline;
}
static BSTR identity_string(AccessibilityUia *uia, IUIAutomationElement *element, uint64_t *hash) {
    VARIANT value;
    LONG first = 0, last = -1, i;
    wchar_t text[1024];
    size_t count;
    BSTR result = NULL;
    VariantInit(&value);
    if (FAILED(IUIAutomationElement_GetCachedPropertyValue(element, UIA_RuntimeIdPropertyId, &value)) ||
        value.vt != (VT_ARRAY | VT_I4) || !value.parray) goto done;
    if (FAILED(SafeArrayGetLBound(value.parray, 1, &first)) || FAILED(SafeArrayGetUBound(value.parray, 1, &last)) ||
        last < first || last - first > 64) goto done;
    count = (size_t)swprintf_s(text, ARRAYSIZE(text), L"%d", cached_int(element, UIA_ProcessIdPropertyId, 0));
    for (i = first; i <= last; ++i) {
        LONG part = 0;
        int written;
        if (FAILED(SafeArrayGetElement(value.parray, &i, &part))) goto done;
        written = swprintf_s(text + count, ARRAYSIZE(text) - count, L":%ld", part);
        if (written <= 0) goto done;
        count += (size_t)written;
    }
    *hash = UINT64_C(14695981039346656037);
    for (i = 0; (size_t)i < count; ++i) { *hash ^= text[i]; *hash *= UINT64_C(1099511628211); }
    result = SysAllocStringLen(text, (UINT)count);
    if (!result) uia->work->error = E_OUTOFMEMORY;
done:
    VariantClear(&value);
    return result;
}
static UiaRecord *find_identity(AccessibilityUia *uia, const wchar_t *identity, uint64_t hash) {
    UiaRecord *record;
    for (record = uia->identities[hash % RECORD_BUCKETS]; record; record = record->identity_next)
        if (record->identity && record->identity_hash == hash && text_equal(record->identity, identity)) return record;
    return NULL;
}
static UiaRecord *find_element(AccessibilityUia *uia, IUIAutomationElement *element, const wchar_t *identity, uint64_t hash) {
    UiaRecord *record = identity ? find_identity(uia, identity, hash) : NULL;
    DWORD pid = (DWORD)cached_int(element, UIA_ProcessIdPropertyId, 0);
    if (record) return record;
    record = identity ? uia->identities[0] : uia->records;
    for (; record; record = identity ? record->identity_next : record->next) {
        BOOL equal = FALSE;
        HRESULT hr;
        if ((identity && record->identity) || record->pid != pid) continue;
        if (!check_work(uia)) return NULL;
        hr = IUIAutomation_CompareElements(uia->automation, record->element, element, &equal);
        if (hr == UIA_E_ELEMENTNOTAVAILABLE) continue;
        if (FAILED(hr)) { uia->work->error = hr; return NULL; }
        if (equal) return record;
    }
    return NULL;
}
static UiaRecord *find_record(AccessibilityUia *uia, const wchar_t *id) {
    wchar_t *end;
    uint64_t number;
    UiaRecord *record;
    if (!id || id[0] != L'w' || id[1] < L'1' || id[1] > L'9') return NULL;
    number = _wcstoui64(id + 1, &end, 10);
    if (*end) return NULL;
    for (record = uia->ids[number % RECORD_BUCKETS]; record; record = record->id_next)
        if (record->number == number && text_equal(record->id, id)) return record;
    return NULL;
}
static UiaRecord *remember(AccessibilityUia *uia, IUIAutomationElement *element, int cached) {
    IUIAutomationElement *updated = NULL;
    UiaRecord *record;
    BSTR identity;
    uint64_t hash = 0;
    if (!element || !check_work(uia)) return NULL;
    if (cached) { updated = element; IUIAutomationElement_AddRef(updated); }
    else {
        uia->work->error = IUIAutomationElement_BuildUpdatedCache(element, uia->cache, &updated);
        if (FAILED(uia->work->error) || !updated) {
            if (SUCCEEDED(uia->work->error)) uia->work->error = UIA_E_ELEMENTNOTAVAILABLE;
            RELEASE(updated); return NULL;
        }
    }
    identity = identity_string(uia, updated, &hash);
    record = check_work(uia) ? find_element(uia, updated, identity, hash) : NULL;
    if (!check_work(uia)) { SysFreeString(identity); RELEASE(updated); return NULL; }
    if (record) {
        if (identity && !record->identity) {
            UiaRecord **bucket = &uia->identities[0];
            while (*bucket != record) bucket = &(*bucket)->identity_next;
            *bucket = record->identity_next;
            record->identity = identity; identity = NULL;
            record->identity_hash = hash;
            record->identity_next = uia->identities[hash % RECORD_BUCKETS];
            uia->identities[hash % RECORD_BUCKETS] = record;
        }
        SysFreeString(identity);
        if (uia->work->capturing) { RELEASE(record->element); record->element = updated; }
        else RELEASE(updated);
        return record;
    }
    if (uia->record_count >= ASB_AX_MAX_NODES * 2u) {
        uia->work->truncated = 1; uia->work->error = HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        SysFreeString(identity); RELEASE(updated); return NULL;
    }
    record = calloc(1, sizeof(*record));
    if (!record) { uia->work->error = E_OUTOFMEMORY; SysFreeString(identity); RELEASE(updated); return NULL; }
    record->element = updated;
    record->identity = identity;
    record->identity_hash = hash;
    record->number = ++uia->next_id;
    swprintf_s(record->id, ARRAYSIZE(record->id), L"w%llu", (unsigned long long)record->number);
    record->pid = (DWORD)cached_int(updated, UIA_ProcessIdPropertyId, 0);
    record->secure = cached_int(updated, UIA_IsPasswordPropertyId, 1) != 0;
    record->identity_next = uia->identities[hash % RECORD_BUCKETS]; uia->identities[hash % RECORD_BUCKETS] = record;
    record->id_next = uia->ids[record->number % RECORD_BUCKETS]; uia->ids[record->number % RECORD_BUCKETS] = record;
    record->next = uia->records; uia->records = record; ++uia->record_count;
    return record;
}
static Json *reference(AccessibilityUia *uia, IUIAutomationElement *element) {
    UiaRecord *record = NULL;
    uint64_t hash = 0;
    BSTR identity;
    if (!element || !check_work(uia)) return ax_json_null();
    identity = identity_string(uia, element, &hash);
    if (!identity && SUCCEEDED(uia->work->error)) {
        IUIAutomationElement *identity_element = NULL;
        if (SUCCEEDED(IUIAutomationElement_BuildUpdatedCache(element, uia->identity_cache, &identity_element)) && identity_element)
            identity = identity_string(uia, identity_element, &hash);
        RELEASE(identity_element);
    }
    if (identity) { record = find_identity(uia, identity, hash); SysFreeString(identity); }
    if (!record) record = remember(uia, element, 0);
    if (!record) return ax_json_null();
    record->reference = uia->capture;
    return element_reference(record);
}
static Json *reference_array(AccessibilityUia *uia, IUIAutomationElementArray *elements) {
    Json *result = ax_json_array();
    int count = 0, i;
    if (!elements || FAILED(IUIAutomationElementArray_get_Length(elements, &count))) return result;
    if (count > ASB_AX_MAX_NODES) { count = ASB_AX_MAX_NODES; uia->work->truncated = 1; }
    for (i = 0; i < count && check_work(uia); ++i) {
        IUIAutomationElement *element = NULL;
        if (SUCCEEDED(IUIAutomationElementArray_GetElement(elements, i, &element))) {
            Json *value = reference(uia, element);
            if (ax_json_type(value) == AX_JSON_NULL) ax_json_release(value);
            else ax_json_append(result, value);
        }
        RELEASE(element);
    }
    return result;
}
static const struct { const long *id; const wchar_t *name; } uia_properties[] = {
    {&UIA_RuntimeIdPropertyId, L"RuntimeId"},
    {&UIA_ProcessIdPropertyId, L"ProcessId"},
    {&UIA_ControlTypePropertyId, L"ControlType"},
    {&UIA_NamePropertyId, L"Name"},
    {&UIA_AutomationIdPropertyId, L"AutomationId"},
    {&UIA_HelpTextPropertyId, L"HelpText"},
    {&UIA_LocalizedControlTypePropertyId, L"LocalizedControlType"},
    {&UIA_BoundingRectanglePropertyId, L"BoundingRectangle"},
    {&UIA_IsEnabledPropertyId, L"IsEnabled"},
    {&UIA_IsOffscreenPropertyId, L"IsOffscreen"},
    {&UIA_HasKeyboardFocusPropertyId, L"HasKeyboardFocus"},
    {&UIA_IsKeyboardFocusablePropertyId, L"IsKeyboardFocusable"},
    {&UIA_IsPasswordPropertyId, L"IsPassword"},
    {&UIA_OrientationPropertyId, L"Orientation"},
    {&UIA_IsInvokePatternAvailablePropertyId, L"IsInvokePatternAvailable"},
    {&UIA_IsTogglePatternAvailablePropertyId, L"IsTogglePatternAvailable"},
    {&UIA_IsSelectionItemPatternAvailablePropertyId, L"IsSelectionItemPatternAvailable"},
    {&UIA_IsSelectionPatternAvailablePropertyId, L"IsSelectionPatternAvailable"},
    {&UIA_IsExpandCollapsePatternAvailablePropertyId, L"IsExpandCollapsePatternAvailable"},
    {&UIA_IsValuePatternAvailablePropertyId, L"IsValuePatternAvailable"},
    {&UIA_IsRangeValuePatternAvailablePropertyId, L"IsRangeValuePatternAvailable"},
    {&UIA_IsTextPatternAvailablePropertyId, L"IsTextPatternAvailable"},
    {&UIA_IsScrollItemPatternAvailablePropertyId, L"IsScrollItemPatternAvailable"},
    {&UIA_IsScrollPatternAvailablePropertyId, L"IsScrollPatternAvailable"},
    {&UIA_IsGridPatternAvailablePropertyId, L"IsGridPatternAvailable"},
    {&UIA_IsGridItemPatternAvailablePropertyId, L"IsGridItemPatternAvailable"},
    {&UIA_IsTablePatternAvailablePropertyId, L"IsTablePatternAvailable"},
    {&UIA_IsTableItemPatternAvailablePropertyId, L"IsTableItemPatternAvailable"},
    {&UIA_IsWindowPatternAvailablePropertyId, L"IsWindowPatternAvailable"},
    {&UIA_IsVirtualizedItemPatternAvailablePropertyId, L"IsVirtualizedItemPatternAvailable"},
    {&UIA_ValueValuePropertyId, L"ValueValue"},
    {&UIA_ValueIsReadOnlyPropertyId, L"ValueIsReadOnly"},
    {&UIA_ToggleToggleStatePropertyId, L"ToggleToggleState"},
    {&UIA_RangeValueValuePropertyId, L"RangeValueValue"},
    {&UIA_RangeValueIsReadOnlyPropertyId, L"RangeValueIsReadOnly"},
    {&UIA_RangeValueMinimumPropertyId, L"RangeValueMinimum"},
    {&UIA_RangeValueMaximumPropertyId, L"RangeValueMaximum"},
    {&UIA_RangeValueSmallChangePropertyId, L"RangeValueSmallChange"},
    {&UIA_RangeValueLargeChangePropertyId, L"RangeValueLargeChange"},
    {&UIA_SelectionItemIsSelectedPropertyId, L"SelectionItemIsSelected"},
    {&UIA_SelectionCanSelectMultiplePropertyId, L"SelectionCanSelectMultiple"},
    {&UIA_SelectionIsSelectionRequiredPropertyId, L"SelectionIsSelectionRequired"},
    {&UIA_SelectionSelectionPropertyId, L"SelectionSelection"},
    {&UIA_TableRowHeadersPropertyId, L"TableRowHeaders"},
    {&UIA_TableColumnHeadersPropertyId, L"TableColumnHeaders"},
    {&UIA_TableItemRowHeaderItemsPropertyId, L"TableItemRowHeaderItems"},
    {&UIA_TableItemColumnHeaderItemsPropertyId, L"TableItemColumnHeaderItems"},
    {&UIA_ExpandCollapseExpandCollapseStatePropertyId, L"ExpandCollapseExpandCollapseState"},
    {&UIA_GridRowCountPropertyId, L"GridRowCount"},
    {&UIA_GridColumnCountPropertyId, L"GridColumnCount"},
    {&UIA_GridItemRowPropertyId, L"GridItemRow"},
    {&UIA_GridItemColumnPropertyId, L"GridItemColumn"},
    {&UIA_GridItemRowSpanPropertyId, L"GridItemRowSpan"},
    {&UIA_GridItemColumnSpanPropertyId, L"GridItemColumnSpan"},
    {&UIA_ScrollHorizontallyScrollablePropertyId, L"ScrollHorizontallyScrollable"},
    {&UIA_ScrollVerticallyScrollablePropertyId, L"ScrollVerticallyScrollable"},
    {&UIA_WindowIsModalPropertyId, L"WindowIsModal"},
    {&UIA_SelectionItemSelectionContainerPropertyId, L"SelectionItemSelectionContainer"},
};

static int decode_number(const Json *value, double *number) {
    *number = ax_json_double(value, NAN);
    return isfinite(*number);
}
static int decode_range(const Json *value, size_t *location, size_t *length) {
    const Json *array = value;
    double a, b;
    if (ax_json_type(array) != AX_JSON_ARRAY || ax_json_count(array) != 2) return 0;
    a = ax_json_double(ax_json_at(array, 0), NAN); b = ax_json_double(ax_json_at(array, 1), NAN);
    if (!isfinite(a) || !isfinite(b) || a < 0 || b < 0 || floor(a) != a || floor(b) != b || a + b > ASB_AX_MAX_STRING) return 0;
    *location = (size_t)a; *length = (size_t)b;
    return 1;
}
static int decode_point(const Json *value, POINT *point) {
    const Json *array = value;
    double x, y;
    if (ax_json_type(array) != AX_JSON_ARRAY || ax_json_count(array) != 2) return 0;
    x = ax_json_double(ax_json_at(array, 0), NAN); y = ax_json_double(ax_json_at(array, 1), NAN);
    if (!isfinite(x) || !isfinite(y) || fabs(x) > 100000000 || fabs(y) > 100000000) return 0;
    point->x = (LONG)x; point->y = (LONG)y;
    return 1;
}
static void free_record(UiaRecord *record) {
    RELEASE(record->element);
    SysFreeString(record->identity); SysFreeString(record->text); SysFreeString(record->captured_text);
    ax_json_release(record->metadata); ax_json_release(record->captured_metadata);
    free(record);
}
void accessibility_uia_reset(AccessibilityUia *uia) {
    UiaRecord *record, *next;
    if (!uia) return;
    accessibility_uia_events_reset(uia->events);
    for (record = uia->records; record; record = next) { next = record->next; free_record(record); }
    uia->records = NULL; uia->record_count = 0; uia->foreground = NULL; uia->event_foreground = NULL;
    if (uia->identities) memset(uia->identities, 0, RECORD_BUCKETS * sizeof(*uia->identities));
    if (uia->ids) memset(uia->ids, 0, RECORD_BUCKETS * sizeof(*uia->ids));
}
void accessibility_uia_destroy(AccessibilityUia *uia) {
    if (!uia) return;
    accessibility_uia_reset(uia);
    accessibility_uia_events_destroy(uia->events);
    RELEASE(uia->walker); RELEASE(uia->cache); RELEASE(uia->children_cache); RELEASE(uia->identity_cache);
    RELEASE(uia->timeouts); RELEASE(uia->automation);
    free(uia->identities); free(uia->ids); free(uia);
}
HRESULT accessibility_uia_create(AccessibilityUia **result) {
    AccessibilityUia *uia;
    IUIAutomationCondition *condition = NULL;
    HRESULT hr = E_OUTOFMEMORY;
    size_t i;
    const PATTERNID patterns[] = {UIA_SelectionPatternId, UIA_SelectionItemPatternId, UIA_TablePatternId, UIA_TableItemPatternId};
    if (!result) return E_POINTER;
    *result = NULL;
    uia = calloc(1, sizeof(*uia));
    if (!uia) return E_OUTOFMEMORY;
    uia->identities = calloc(RECORD_BUCKETS, sizeof(*uia->identities));
    uia->ids = calloc(RECORD_BUCKETS, sizeof(*uia->ids));
    if (!uia->identities || !uia->ids) goto done;
    ProcessIdToSessionId(GetCurrentProcessId(), &uia->session_id);
    TRY(CoCreateInstance(&CLSID_CUIAutomation8, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&uia->automation));
    if (SUCCEEDED(IUIAutomation_QueryInterface(uia->automation, &IID_IUIAutomation2, (void **)&uia->timeouts))) {
        IUIAutomation2_put_ConnectionTimeout(uia->timeouts, 500);
        IUIAutomation2_put_TransactionTimeout(uia->timeouts, 1000);
    }
    TRY(IUIAutomation_CreateCacheRequest(uia->automation, &uia->cache));
    TRY(IUIAutomationCacheRequest_put_TreeScope(uia->cache, TreeScope_Element));
    TRY(IUIAutomationCacheRequest_put_AutomationElementMode(uia->cache, AutomationElementMode_Full));
    TRY(IUIAutomation_get_ControlViewCondition(uia->automation, &condition));
    TRY(IUIAutomationCacheRequest_put_TreeFilter(uia->cache, condition));
    for (i = 0; i < ARRAYSIZE(uia_properties); ++i) TRY(IUIAutomationCacheRequest_AddProperty(uia->cache, *uia_properties[i].id));
    for (i = 0; i < ARRAYSIZE(patterns); ++i) TRY(IUIAutomationCacheRequest_AddPattern(uia->cache, patterns[i]));
    TRY(IUIAutomation_CreateCacheRequest(uia->automation, &uia->identity_cache));
    TRY(IUIAutomationCacheRequest_put_TreeScope(uia->identity_cache, TreeScope_Element));
    TRY(IUIAutomationCacheRequest_put_AutomationElementMode(uia->identity_cache, AutomationElementMode_None));
    TRY(IUIAutomationCacheRequest_AddProperty(uia->identity_cache, UIA_RuntimeIdPropertyId));
    TRY(IUIAutomationCacheRequest_AddProperty(uia->identity_cache, UIA_ProcessIdPropertyId));
    TRY(IUIAutomationCacheRequest_Clone(uia->identity_cache, &uia->children_cache));
    TRY(IUIAutomationCacheRequest_put_TreeScope(uia->children_cache, TreeScope_Children));
    TRY(IUIAutomationCacheRequest_put_AutomationElementMode(uia->children_cache, AutomationElementMode_Full));
    TRY(IUIAutomationCacheRequest_put_TreeFilter(uia->children_cache, condition));
    TRY(IUIAutomation_get_ControlViewWalker(uia->automation, &uia->walker));
    TRY(accessibility_uia_events_create(uia->automation, &uia->events));
    *result = uia; uia = NULL;
done:
    RELEASE(condition);
    accessibility_uia_destroy(uia);
    return hr;
}
int accessibility_uia_active(const AccessibilityUia *uia) {
    HDESK desktop;
    wchar_t name[128];
    DWORD needed = 0;
    int active;
    if (uia->session_id != WTSGetActiveConsoleSessionId()) return 0;
    desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return 0;
    active = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) && _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return active;
}
int accessibility_uia_foreground_changed(const AccessibilityUia *uia) { return uia->foreground != GetForegroundWindow(); }
uint64_t accessibility_uia_change_generation(const AccessibilityUia *uia) { return accessibility_uia_events_generation(uia->events); }
uint64_t accessibility_uia_last_change_time(const AccessibilityUia *uia) { return accessibility_uia_events_last_change_time(uia->events); }

static int read_text(IUIAutomationTextRange *range, BSTR *text) {
    *text = NULL;
    return SUCCEEDED(IUIAutomationTextRange_GetText(range, ASB_AX_MAX_STRING + 1, text));
}
static void add_text(AccessibilityUia *uia, UiaRecord *record, Json *properties) {
    IUIAutomationTextPattern *pattern = NULL;
    IUIAutomationTextRange *document = NULL, *selected = NULL, *prefix = NULL;
    IUIAutomationTextRangeArray *selection = NULL;
    BSTR text = NULL, before = NULL, chosen = NULL;
    enum SupportedTextSelection supported = SupportedTextSelection_None;
    int count = 0, i;
    size_t length;
    Json *value, *ranges = NULL;
    record->captured_complete_text = 0;
    SysFreeString(record->captured_text); record->captured_text = NULL;
    if (!check_work(uia) || record->secure || !cached_int(record->element, UIA_IsTextPatternAvailablePropertyId, 0)) return;
    if (FAILED(PATTERN(record->element, IUIAutomationTextPattern, UIA_TextPatternId, &pattern)) || !pattern ||
        FAILED(IUIAutomationTextPattern_get_DocumentRange(pattern, &document)) || !document || !read_text(document, &text)) goto done;
    value = snapshot_string(uia, text ? text : L"", SysStringLen(text));
    length = ax_json_length(value);
    record->captured_text = SysAllocStringLen(ax_json_text(value), (UINT)length);
    if (!record->captured_text) { uia->work->error = E_OUTOFMEMORY; ax_json_release(value); goto done; }
    ax_json_set(properties, L"Text.DocumentText", value);
    if (length != SysStringLen(text)) { uia->work->truncated = 1; goto done; }
    record->captured_complete_text = 1;
    if (FAILED(IUIAutomationTextPattern_get_SupportedTextSelection(pattern, &supported))) goto done;
    ax_json_set(properties, L"Text.SupportedTextSelection", ax_json_number(supported));
    if (supported == SupportedTextSelection_None || FAILED(IUIAutomationTextPattern_GetSelection(pattern, &selection)) ||
        !selection || FAILED(IUIAutomationTextRangeArray_get_Length(selection, &count)) || count > ASB_AX_MAX_NODES) goto done;
    ranges = ax_json_array();
    for (i = 0; i < count && check_work(uia); ++i) {
        if (FAILED(IUIAutomationTextRangeArray_GetElement(selection, i, &selected)) ||
            FAILED(IUIAutomationTextRange_Clone(document, &prefix)) ||
            FAILED(IUIAutomationTextRange_MoveEndpointByRange(prefix, TextPatternRangeEndpoint_End, selected, TextPatternRangeEndpoint_Start)) ||
            !read_text(prefix, &before) || !read_text(selected, &chosen) ||
            (size_t)SysStringLen(before) + SysStringLen(chosen) > length ||
            memcmp(text + SysStringLen(before), chosen, SysStringLen(chosen) * sizeof(wchar_t))) goto done;
        ax_json_append(ranges, range_value(SysStringLen(before), SysStringLen(chosen)));
        RELEASE(prefix); RELEASE(selected); SysFreeString(before); before = NULL; SysFreeString(chosen); chosen = NULL;
    }
    ax_json_set(properties, L"Text.Selection", ranges); ranges = NULL;
done:
    ax_json_release(ranges); SysFreeString(text); SysFreeString(before); SysFreeString(chosen);
    RELEASE(prefix); RELEASE(selected); RELEASE(selection); RELEASE(document); RELEASE(pattern);
}
static void add_relations(AccessibilityUia *uia, UiaRecord *record, Json *attributes) {
    IUIAutomationSelectionPattern *selection = NULL;
    IUIAutomationTablePattern *table = NULL;
    IUIAutomationTableItemPattern *item = NULL;
    IUIAutomationElementArray *elements = NULL;
    IUIAutomationElement *element = record->element;
    if (cached_int(element, UIA_IsSelectionPatternAvailablePropertyId, 0) &&
        SUCCEEDED(CACHED_PATTERN(element, IUIAutomationSelectionPattern, UIA_SelectionPatternId, &selection)) && selection &&
        SUCCEEDED(IUIAutomationSelectionPattern_GetCachedSelection(selection, &elements))) {
        Json *value = reference_array(uia, elements);
        ax_json_set(attributes, L"SelectionSelection", value);
    }
    RELEASE(elements); RELEASE(selection);
    if (cached_int(element, UIA_IsTablePatternAvailablePropertyId, 0) &&
        SUCCEEDED(CACHED_PATTERN(element, IUIAutomationTablePattern, UIA_TablePatternId, &table)) && table) {
        if (SUCCEEDED(IUIAutomationTablePattern_GetCachedColumnHeaders(table, &elements)))
            ax_json_set(attributes, L"TableColumnHeaders", reference_array(uia, elements));
        RELEASE(elements);
        if (SUCCEEDED(IUIAutomationTablePattern_GetCachedRowHeaders(table, &elements)))
            ax_json_set(attributes, L"TableRowHeaders", reference_array(uia, elements));
        RELEASE(elements);
    }
    RELEASE(table);
    if (cached_int(element, UIA_IsTableItemPatternAvailablePropertyId, 0) &&
        SUCCEEDED(CACHED_PATTERN(element, IUIAutomationTableItemPattern, UIA_TableItemPatternId, &item)) && item) {
        if (SUCCEEDED(IUIAutomationTableItemPattern_GetCachedColumnHeaderItems(item, &elements)))
            ax_json_set(attributes, L"TableItemColumnHeaderItems", reference_array(uia, elements));
        RELEASE(elements);
        if (SUCCEEDED(IUIAutomationTableItemPattern_GetCachedRowHeaderItems(item, &elements)))
            ax_json_set(attributes, L"TableItemRowHeaderItems", reference_array(uia, elements));
        RELEASE(elements);
    }
    RELEASE(item);
    if (cached_int(element, UIA_IsSelectionItemPatternAvailablePropertyId, 0)) {
        IUIAutomationSelectionItemPattern *selected = NULL;
        IUIAutomationElement *container = NULL;
        if (SUCCEEDED(CACHED_PATTERN(element, IUIAutomationSelectionItemPattern, UIA_SelectionItemPatternId, &selected)) && selected &&
            SUCCEEDED(IUIAutomationSelectionItemPattern_get_CachedSelectionContainer(selected, &container)) && container)
            ax_json_set(attributes, L"SelectionItemSelectionContainer", reference(uia, container));
        RELEASE(container); RELEASE(selected);
    }
}
static Json *frame_value(RECT bounds) {
    double values[] = {(double)bounds.left, (double)bounds.top,
        fmax(0, (double)bounds.right - bounds.left), fmax(0, (double)bounds.bottom - bounds.top)};
    return numbers(values, 4);
}
static Json *metadata(AccessibilityUia *uia, UiaRecord *record, int include_details) {
    Json *node = ax_json_object(), *values = ax_json_object();
    size_t i;
    RECT bounds = {0};
    if (!node || !values) { ax_json_release(node); ax_json_release(values); uia->work->error = E_OUTOFMEMORY; return NULL; }
    record->secure = cached_int(record->element, UIA_IsPasswordPropertyId, 1) != 0;
    ax_json_set(node, L"id", ax_json_string(record->id));
    ax_json_set(node, L"children", ax_json_array());
    for (i = 0; i < ARRAYSIZE(uia_properties) && check_work(uia); ++i) {
        VARIANT value;
        Json *encoded = NULL;
        VariantInit(&value);
        if (SUCCEEDED(IUIAutomationElement_GetCachedPropertyValue(record->element, *uia_properties[i].id, &value))) {
            int hidden = record->secure && (*uia_properties[i].id == UIA_RangeValueValuePropertyId ||
                (*uia_properties[i].id == UIA_ValueValuePropertyId && (value.vt != VT_BSTR || !is_masked_value(value.bstrVal))));
            if (!hidden) {
                if (value.vt == VT_BSTR) encoded = snapshot_string(uia, value.bstrVal ? value.bstrVal : L"", SysStringLen(value.bstrVal));
                else if (value.vt == VT_BOOL) encoded = ax_json_boolean(value.boolVal != VARIANT_FALSE);
                else if (value.vt == VT_I4) encoded = ax_json_number(value.lVal);
                else if (value.vt == VT_R8 && isfinite(value.dblVal)) encoded = ax_json_number(value.dblVal);
            }
        }
        VariantClear(&value);
        if (encoded) ax_json_set(values, uia_properties[i].name, encoded);
    }
    ax_json_set(values, L"IsPassword", ax_json_boolean(record->secure));
    if (SUCCEEDED(IUIAutomationElement_get_CachedBoundingRectangle(record->element, &bounds)))
        ax_json_set(values, L"BoundingRectangle", frame_value(bounds));
    if (include_details) { add_text(uia, record, values); add_relations(uia, record, values); }
    ax_json_set(node, L"properties", values);
    return node;
}

static IUIAutomationTextRange *text_prefix(IUIAutomationTextRange *document, size_t offset, uint64_t deadline) {
    int low = 0, high = (int)offset;
    while (low <= high && GetTickCount64() < deadline) {
        int units = low + (high - low) / 2, moved = 0;
        IUIAutomationTextRange *range = NULL;
        BSTR text = NULL;
        size_t length;
        if (FAILED(IUIAutomationTextRange_Clone(document, &range)) || !range ||
            FAILED(IUIAutomationTextRange_MoveEndpointByRange(range, TextPatternRangeEndpoint_End, document, TextPatternRangeEndpoint_Start)) ||
            FAILED(IUIAutomationTextRange_MoveEndpointByUnit(range, TextPatternRangeEndpoint_End, TextUnit_Character, units, &moved)) ||
            !read_text(range, &text)) { RELEASE(range); SysFreeString(text); return NULL; }
        length = SysStringLen(text); SysFreeString(text);
        if (length == offset) return range;
        RELEASE(range);
        if (length < offset) low = units + 1; else high = units - 1;
    }
    return NULL;
}
static HRESULT text_bounds(AccessibilityUia *uia, IUIAutomationTextRange *range, Json **result) {
    SAFEARRAY *rectangles = NULL;
    HRESULT hr;
    LONG first = 0, last = -1, i;
    Json *values = ax_json_array();
    TRY(IUIAutomationTextRange_GetBoundingRectangles(range, &rectangles));
    if (!rectangles) { hr = UIA_E_NOTSUPPORTED; goto done; }
    TRY(SafeArrayGetLBound(rectangles, 1, &first)); TRY(SafeArrayGetUBound(rectangles, 1, &last));
    if (last - first >= 65536 || (last - first + 1) % 4) { hr = UIA_E_NOTSUPPORTED; goto done; }
    for (i = first; i <= last; ++i) {
        double number;
        if (!check_work(uia)) { hr = uia->work->error; goto done; }
        TRY(SafeArrayGetElement(rectangles, &i, &number));
        if (!isfinite(number)) { hr = UIA_E_NOTSUPPORTED; goto done; }
        ax_json_append(values, ax_json_number(number));
    }
    if (!ax_json_valid(values)) { hr = E_OUTOFMEMORY; goto done; }
    *result = values; values = NULL; hr = S_OK;
done:
    if (rectangles) SafeArrayDestroy(rectangles);
    ax_json_release(values); return hr;
}
static Json *text_format_attributes(IUIAutomationTextRange *range) {
    Json *attributes = ax_json_object();
    size_t i;
    const struct { TEXTATTRIBUTEID id; const wchar_t *name; } fields[] = {
        {UIA_FontNameAttributeId, L"FontName"}, {UIA_FontSizeAttributeId, L"FontSize"},
        {UIA_FontWeightAttributeId, L"FontWeight"}, {UIA_IsItalicAttributeId, L"IsItalic"},
        {UIA_UnderlineStyleAttributeId, L"UnderlineStyle"}, {UIA_StrikethroughStyleAttributeId, L"StrikethroughStyle"}
    };
    for (i = 0; i < ARRAYSIZE(fields); ++i) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(IUIAutomationTextRange_GetAttributeValue(range, fields[i].id, &value))) {
            if (value.vt == VT_BSTR) ax_json_set(attributes, fields[i].name,
                ax_json_string_n(value.bstrVal, limited_length(value.bstrVal, SysStringLen(value.bstrVal), ASB_AX_MAX_STRING)));
            else if (value.vt == VT_R8 && isfinite(value.dblVal)) ax_json_set(attributes, fields[i].name, ax_json_number(value.dblVal));
            else if (value.vt == VT_I4) ax_json_set(attributes, fields[i].name, ax_json_number(value.lVal));
            else if (value.vt == VT_BOOL) ax_json_set(attributes, fields[i].name, ax_json_boolean(value.boolVal != VARIANT_FALSE));
        }
        VariantClear(&value);
    }
    return attributes;
}
static HRESULT text_attributed(AccessibilityUia *uia, IUIAutomationTextRange *document,
    const wchar_t *text, size_t length, uint64_t deadline, Json **result) {
    IUIAutomationTextRange *cursor = NULL;
    Json *runs = ax_json_array();
    BSTR chosen = NULL;
    size_t offset = 0;
    HRESULT hr;
    TRY(IUIAutomationTextRange_Clone(document, &cursor));
    while (offset < length && ax_json_count(runs) < 4096) {
        int comparison = 0;
        Json *run;
        size_t count;
        if (!prepare(uia, deadline)) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
        TRY(IUIAutomationTextRange_ExpandToEnclosingUnit(cursor, TextUnit_Format));
        TRY(IUIAutomationTextRange_CompareEndpoints(cursor, TextPatternRangeEndpoint_Start, document, TextPatternRangeEndpoint_Start, &comparison));
        if (comparison < 0) TRY(IUIAutomationTextRange_MoveEndpointByRange(cursor, TextPatternRangeEndpoint_Start, document, TextPatternRangeEndpoint_Start));
        TRY(IUIAutomationTextRange_CompareEndpoints(cursor, TextPatternRangeEndpoint_End, document, TextPatternRangeEndpoint_End, &comparison));
        if (comparison > 0) TRY(IUIAutomationTextRange_MoveEndpointByRange(cursor, TextPatternRangeEndpoint_End, document, TextPatternRangeEndpoint_End));
        if (!read_text(cursor, &chosen) || !(count = SysStringLen(chosen)) || count > length - offset) { hr = UIA_E_NOTSUPPORTED; goto done; }
        run = ax_json_object();
        ax_json_set(run, L"range", pair((double)offset, (double)count));
        ax_json_set(run, L"attributes", text_format_attributes(cursor));
        ax_json_append(runs, run);
        offset += count; SysFreeString(chosen); chosen = NULL;
        if (offset < length) TRY(IUIAutomationTextRange_MoveEndpointByRange(cursor, TextPatternRangeEndpoint_Start, cursor, TextPatternRangeEndpoint_End));
    }
    if (offset != length) { hr = UIA_E_NOTSUPPORTED; goto done; }
    *result = ax_json_object();
    ax_json_set(*result, L"text", ax_json_string_n(text, length));
    ax_json_set(*result, L"runs", runs); runs = NULL;
    hr = ax_json_valid(*result) ? S_OK : E_OUTOFMEMORY;
done:
    SysFreeString(chosen); RELEASE(cursor); ax_json_release(runs);
    return hr;
}
static HRESULT text_request(AccessibilityUia *uia, UiaRecord *record, const wchar_t *attribute,
    const Json *parameter, int setter, uint64_t deadline, Json **result) {
    IUIAutomationTextPattern *pattern = NULL;
    IUIAutomationTextRange *document = NULL, *start = NULL, *end = NULL, *at = NULL, *prefix = NULL;
    BSTR text = NULL, before = NULL;
    size_t location = 0, length = 0, count;
    HRESULT hr = UIA_E_NOTSUPPORTED;
    const wchar_t *contents;
    *result = NULL;
    if (record->secure || !record->complete_text || !prepare(uia, deadline)) return hr;
    if (FAILED(PATTERN(record->element, IUIAutomationTextPattern, UIA_TextPatternId, &pattern)) || !pattern) goto done;
    TRY(IUIAutomationTextPattern_get_DocumentRange(pattern, &document));
    if (!document || !read_text(document, &text) || SysStringLen(text) > ASB_AX_MAX_STRING) { hr = UIA_E_NOTSUPPORTED; goto done; }
    count = SysStringLen(text); contents = text ? text : L"";
    if (setter && (count != SysStringLen(record->text) || (count && memcmp(contents, record->text, count * sizeof(wchar_t))))) {
        hr = UIA_E_ELEMENTNOTAVAILABLE; goto done;
    }
    if (text_equal(attribute, L"Text.RangeFromPoint")) {
        POINT point;
        if (!decode_point(parameter, &point)) { hr = E_INVALIDARG; goto done; }
        TRY(IUIAutomationTextPattern_RangeFromPoint(pattern, point, &at));
        TRY(IUIAutomationTextRange_Clone(document, &prefix));
        TRY(IUIAutomationTextRange_MoveEndpointByRange(prefix, TextPatternRangeEndpoint_End, at, TextPatternRangeEndpoint_Start));
        if (!read_text(prefix, &before)) { hr = UIA_E_NOTSUPPORTED; goto done; }
        *result = range_value(SysStringLen(before), 0); hr = S_OK; goto done;
    }
    if (!decode_range(parameter, &location, &length) || location + length > count) { hr = E_INVALIDARG; goto done; }
    start = text_prefix(document, location, deadline); end = text_prefix(document, location + length, deadline);
    if (!start || !end) { hr = UIA_E_NOTSUPPORTED; goto done; }
    TRY(IUIAutomationTextRange_MoveEndpointByRange(document, TextPatternRangeEndpoint_Start, start, TextPatternRangeEndpoint_End));
    TRY(IUIAutomationTextRange_MoveEndpointByRange(document, TextPatternRangeEndpoint_End, end, TextPatternRangeEndpoint_End));
    if (setter) hr = prepare(uia, deadline) ? IUIAutomationTextRange_Select(document) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    else if (text_equal(attribute, L"Text.GetText")) { *result = ax_json_string_n(contents + location, length); hr = S_OK; }
    else if (text_equal(attribute, L"Text.GetBoundingRectangles")) hr = text_bounds(uia, document, result);
    else if (text_equal(attribute, L"Text.GetAttributeRuns")) hr = text_attributed(uia, document, contents + location, length, deadline, result);
    else hr = UIA_E_NOTSUPPORTED;
done:
    SysFreeString(text); SysFreeString(before);
    RELEASE(prefix); RELEASE(at); RELEASE(end); RELEASE(start); RELEASE(document); RELEASE(pattern);
    if (!setter && SUCCEEDED(hr) && !ax_json_valid(*result)) hr = E_OUTOFMEMORY;
    if (FAILED(hr)) { ax_json_release(*result); *result = NULL; }
    return hr;
}
static HRESULT native_operation(AccessibilityUia *uia, UiaRecord *record, const wchar_t *action,
    const Json *value, uint64_t deadline) {
    IUIAutomationElement *element = record->element;
    IUIAutomationInvokePattern *invoke = NULL;
    IUIAutomationTogglePattern *toggle = NULL;
    IUIAutomationSelectionItemPattern *selection = NULL;
    IUIAutomationExpandCollapsePattern *expand = NULL;
    IUIAutomationScrollPattern *scroll = NULL;
    IUIAutomationScrollItemPattern *scroll_item = NULL;
    IUIAutomationVirtualizedItemPattern *virtualized = NULL;
    IUIAutomationRangeValuePattern *range = NULL;
    IUIAutomationValuePattern *text = NULL;
    HRESULT hr = UIA_E_NOTSUPPORTED;
    BOOL read_only = TRUE;
    BSTR input = NULL;
    if (!prepare(uia, deadline)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (text_equal(action, L"SetFocus")) return IUIAutomationElement_SetFocus(element);
    if (text_equal(action, L"Invoke.Invoke")) {
        if (FAILED(PATTERN(element, IUIAutomationInvokePattern, UIA_InvokePatternId, &invoke)) || !invoke) goto done;
        hr = prepare(uia, deadline) ? IUIAutomationInvokePattern_Invoke(invoke) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"Toggle.Toggle")) {
        if (FAILED(PATTERN(element, IUIAutomationTogglePattern, UIA_TogglePatternId, &toggle)) || !toggle) goto done;
        hr = prepare(uia, deadline) ? IUIAutomationTogglePattern_Toggle(toggle) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"SelectionItem.Select") || text_equal(action, L"SelectionItem.AddToSelection") ||
        text_equal(action, L"SelectionItem.RemoveFromSelection")) {
        if (FAILED(PATTERN(element, IUIAutomationSelectionItemPattern, UIA_SelectionItemPatternId, &selection)) || !selection) goto done;
        if (!prepare(uia, deadline)) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
        hr = text_equal(action, L"SelectionItem.Select") ? IUIAutomationSelectionItemPattern_Select(selection) :
            text_equal(action, L"SelectionItem.AddToSelection") ? IUIAutomationSelectionItemPattern_AddToSelection(selection) :
            IUIAutomationSelectionItemPattern_RemoveFromSelection(selection);
    } else if (text_equal(action, L"ExpandCollapse.Expand") || text_equal(action, L"ExpandCollapse.Collapse")) {
        if (FAILED(PATTERN(element, IUIAutomationExpandCollapsePattern, UIA_ExpandCollapsePatternId, &expand)) || !expand) goto done;
        if (!prepare(uia, deadline)) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
        hr = text_equal(action, L"ExpandCollapse.Expand") ? IUIAutomationExpandCollapsePattern_Expand(expand) :
            IUIAutomationExpandCollapsePattern_Collapse(expand);
    } else if (text_equal(action, L"Scroll.Scroll")) {
        double horizontal, vertical;
        if (ax_json_type(value) != AX_JSON_ARRAY || ax_json_count(value) != 2 ||
            !decode_number(ax_json_at(value, 0), &horizontal) || !decode_number(ax_json_at(value, 1), &vertical) ||
            horizontal < ScrollAmount_LargeDecrement || horizontal > ScrollAmount_SmallIncrement || floor(horizontal) != horizontal ||
            vertical < ScrollAmount_LargeDecrement || vertical > ScrollAmount_SmallIncrement || floor(vertical) != vertical) { hr = E_INVALIDARG; goto done; }
        if (FAILED(PATTERN(element, IUIAutomationScrollPattern, UIA_ScrollPatternId, &scroll)) || !scroll) goto done;
        hr = prepare(uia, deadline) ? IUIAutomationScrollPattern_Scroll(scroll, (enum ScrollAmount)horizontal, (enum ScrollAmount)vertical) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"ScrollItem.ScrollIntoView")) {
        if (FAILED(PATTERN(element, IUIAutomationScrollItemPattern, UIA_ScrollItemPatternId, &scroll_item)) || !scroll_item) goto done;
        hr = prepare(uia, deadline) ? IUIAutomationScrollItemPattern_ScrollIntoView(scroll_item) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"VirtualizedItem.Realize")) {
        if (FAILED(PATTERN(element, IUIAutomationVirtualizedItemPattern, UIA_VirtualizedItemPatternId, &virtualized)) || !virtualized) goto done;
        hr = prepare(uia, deadline) ? IUIAutomationVirtualizedItemPattern_Realize(virtualized) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"RangeValue.SetValue")) {
        double number, minimum = 0, maximum = 0;
        if (!decode_number(value, &number)) { hr = E_INVALIDARG; goto done; }
        if (FAILED(PATTERN(element, IUIAutomationRangeValuePattern, UIA_RangeValuePatternId, &range)) || !range) goto done;
        TRY(IUIAutomationRangeValuePattern_get_CurrentIsReadOnly(range, &read_only));
        if (read_only) { hr = UIA_E_NOTSUPPORTED; goto done; }
        TRY(IUIAutomationRangeValuePattern_get_CurrentMinimum(range, &minimum));
        TRY(IUIAutomationRangeValuePattern_get_CurrentMaximum(range, &maximum));
        if (!isfinite(minimum) || !isfinite(maximum) || number < minimum || number > maximum) { hr = E_INVALIDARG; goto done; }
        hr = prepare(uia, deadline) ? IUIAutomationRangeValuePattern_SetValue(range, number) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else if (text_equal(action, L"Value.SetValue")) {
        if (ax_json_type(value) != AX_JSON_STRING || ax_json_length(value) > ASB_AX_MAX_STRING ||
            wmemchr(ax_json_text(value), 0, ax_json_length(value))) { hr = E_INVALIDARG; goto done; }
        if (FAILED(PATTERN(element, IUIAutomationValuePattern, UIA_ValuePatternId, &text)) || !text) goto done;
        TRY(IUIAutomationValuePattern_get_CurrentIsReadOnly(text, &read_only));
        if (read_only) { hr = UIA_E_NOTSUPPORTED; goto done; }
        input = SysAllocStringLen(ax_json_text(value), (UINT)ax_json_length(value));
        if (!input) { hr = E_OUTOFMEMORY; goto done; }
        hr = prepare(uia, deadline) ? IUIAutomationValuePattern_SetValue(text, input) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
done:
    SysFreeString(input); RELEASE(text); RELEASE(range); RELEASE(virtualized); RELEASE(scroll_item);
    RELEASE(scroll); RELEASE(expand); RELEASE(selection); RELEASE(toggle); RELEASE(invoke);
    return hr;
}

typedef struct CaptureWindows {
    HWND owner, windows[64];
    size_t count;
    int overflow;
} CaptureWindows;
static BOOL CALLBACK collect_owned_window(HWND window, LPARAM parameter) {
    CaptureWindows *context = (CaptureWindows *)parameter;
    if (window != context->windows[0] && IsWindowVisible(window) && !IsIconic(window) && GetAncestor(window, GA_ROOTOWNER) == context->owner) {
        if (context->count == ARRAYSIZE(context->windows)) { context->overflow = 1; return FALSE; }
        context->windows[context->count++] = window;
    }
    return TRUE;
}
static void prune_records(AccessibilityUia *uia) {
    UiaRecord **link = &uia->records;
    while (*link) {
        UiaRecord *record = *link, **bucket;
        if (record->capture == uia->capture || record->reference == uia->capture) { link = &record->next; continue; }
        bucket = &uia->identities[record->identity_hash % RECORD_BUCKETS];
        while (*bucket != record) bucket = &(*bucket)->identity_next;
        *bucket = record->identity_next;
        bucket = &uia->ids[record->number % RECORD_BUCKETS];
        while (*bucket != record) bucket = &(*bucket)->id_next;
        *bucket = record->id_next;
        *link = record->next; --uia->record_count; free_record(record);
    }
}
HRESULT accessibility_uia_capture(AccessibilityUia *uia, uint64_t deadline, AccessibilityContinue continuation, void *context, Json **result) {
    typedef struct Pending { IUIAutomationElement *element; UiaRecord *parent; unsigned depth; } Pending;
    Pending *queue = NULL;
    UiaRecord **captured = NULL, *record;
    size_t head = 0, queued = 0, count = 0, i;
    unsigned pass;
    HWND window;
    DWORD pid = 0;
    MONITORINFO monitor = {sizeof(monitor)};
    CaptureWindows windows = {0};
    IUIAutomationElement *root = NULL, *expanded = NULL, *focus = NULL;
    IUIAutomationElementArray *children = NULL;
    Json *output = NULL, *roots = NULL, *snapshot = NULL;
    UiaWork work = {0}, *previous = uia->work;
    HRESULT status;
    int success = 0;
    if (!result) return E_POINTER;
    *result = NULL;
    uia->work = &work; work.capturing = 1;
    output = ax_json_array(); roots = ax_json_array();
    uia->work->continuation = continuation; uia->work->continuation_context = context;
    uia->work->deadline = deadline; uia->work->error = S_OK;
    uia->work->truncated = 0;
    if (uia->timeouts && FAILED(uia->work->error = IUIAutomation2_put_TransactionTimeout(uia->timeouts, 1000))) goto done;
    if (!accessibility_uia_active(uia) || !(window = GetForegroundWindow())) {
        uia->work->error = HRESULT_FROM_WIN32(ERROR_CANCELLED); goto done;
    }
    GetWindowThreadProcessId(window, &pid);
    if (!pid || !GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) goto done;
    windows.owner = GetAncestor(window, GA_ROOTOWNER); windows.windows[0] = window; windows.count = 1;
    EnumWindows(collect_owned_window, (LPARAM)&windows);
    uia->work->truncated = windows.overflow; uia->work->text_units = 0; ++uia->capture;
    for (record = uia->records; record; record = record->next) {
        ax_json_release(record->captured_metadata); record->captured_metadata = NULL;
        SysFreeString(record->captured_text); record->captured_text = NULL; record->captured_complete_text = 0;
    }
    queue = calloc(ASB_AX_MAX_NODES, sizeof(*queue)); captured = calloc(ASB_AX_MAX_NODES, sizeof(*captured));
    if (!queue || !captured || !output || !roots) { uia->work->error = E_OUTOFMEMORY; goto done; }
    for (i = 0; i < windows.count; ++i) {
        if (!continue_capture(uia)) goto done;
        uia->work->error = IUIAutomation_ElementFromHandleBuildCache(uia->automation, windows.windows[i], uia->cache, &root);
        if (i && uia->work->error == UIA_E_ELEMENTNOTAVAILABLE) { RELEASE(root); uia->work->error = S_OK; continue; }
        if (FAILED(uia->work->error) || !root) goto done;
        if (windows.windows[i] == window && window != uia->event_foreground) {
            uia->event_foreground = NULL;
            if (FAILED(uia->work->error = accessibility_uia_events_observe(uia->events, root))) goto done;
            uia->event_foreground = window;
        }
        queue[queued++].element = root; root = NULL;
    }
    while (queued && count < ASB_AX_MAX_NODES) {
        Pending item;
        int child_count = 0, index;
        if (!continue_capture(uia)) goto done;
        item = queue[head]; queue[head].element = NULL; head = (head + 1) % ASB_AX_MAX_NODES; --queued;
        record = remember(uia, item.element, item.parent == NULL); RELEASE(item.element);
        if (!record) goto done;
        if (record->capture == uia->capture) continue;
        record->capture = uia->capture;
        record->captured_metadata = metadata(uia, record, 0);
        if (!record->captured_metadata || !check_work(uia)) goto done;
        if (item.parent) {
            ax_json_set(record->captured_metadata, L"parent", ax_json_string(item.parent->id));
            ax_json_append(named(item.parent->captured_metadata, L"children"), ax_json_string(record->id));
        } else ax_json_append(roots, ax_json_string(record->id));
        captured[count++] = record;
        ax_json_append(output, ax_json_retain(record->captured_metadata));
        if (item.depth >= 127) { uia->work->truncated = 1; continue; }
        if (FAILED(uia->work->error = IUIAutomationElement_BuildUpdatedCache(record->element, uia->children_cache, &expanded)) || !expanded) goto done;
        if (FAILED(uia->work->error = IUIAutomationElement_GetCachedChildren(expanded, &children))) goto done;
        if (children) {
            if (FAILED(uia->work->error = IUIAutomationElementArray_get_Length(children, &child_count))) goto done;
            for (index = 0; index < child_count; ++index) {
                IUIAutomationElement *child = NULL;
                size_t tail;
                if (count + queued >= ASB_AX_MAX_NODES) { uia->work->truncated = 1; break; }
                if (!check_work(uia)) goto done;
                uia->work->error = IUIAutomationElementArray_GetElement(children, index, &child);
                if (uia->work->error == UIA_E_ELEMENTNOTAVAILABLE) { RELEASE(child); uia->work->error = S_OK; continue; }
                if (FAILED(uia->work->error) || !child) { RELEASE(child); goto done; }
                tail = (head + queued) % ASB_AX_MAX_NODES;
                queue[tail].element = child; queue[tail].parent = record; queue[tail].depth = item.depth + 1; ++queued;
            }
        }
        RELEASE(children); RELEASE(expanded);
    }
    if (queued) uia->work->truncated = 1;
    for (i = 0; i < count; ++i) {
        if (!continue_capture(uia)) goto done;
        add_relations(uia, captured[i], named(captured[i]->captured_metadata, L"properties"));
    }
    for (pass = 0; pass < 2; ++pass) for (i = 0; i < count; ++i) {
        int focused;
        if (!continue_capture(uia)) goto done;
        record = captured[i]; focused = cached_int(record->element, UIA_HasKeyboardFocusPropertyId, 0) != 0;
        if ((pass == 0) != focused || record->secure) continue;
        add_text(uia, record, named(record->captured_metadata, L"properties"));
    }
    if (!count) goto done;
    if (!accessibility_uia_active(uia) || GetForegroundWindow() != window) {
        uia->work->error = HRESULT_FROM_WIN32(ERROR_CANCELLED); goto done;
    }
    {
        wchar_t path[1024] = {0};
        const wchar_t *base;
        DWORD length = ARRAYSIZE(path);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        Json *app = ax_json_object(), *display = ax_json_object();
        if (process) { QueryFullProcessImageNameW(process, 0, path, &length); CloseHandle(process); }
        base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        ax_json_set(app, L"pid", ax_json_number(pid)); ax_json_set(app, L"name", ax_json_string(*base ? base : L"Windows"));
        ax_json_set(app, L"processPath", ax_json_string(path));
        ax_json_set(display, L"x", ax_json_number(monitor.rcMonitor.left)); ax_json_set(display, L"y", ax_json_number(monitor.rcMonitor.top));
        ax_json_set(display, L"width", ax_json_number((double)monitor.rcMonitor.right - monitor.rcMonitor.left));
        ax_json_set(display, L"height", ax_json_number((double)monitor.rcMonitor.bottom - monitor.rcMonitor.top));
        ax_json_set(display, L"pixelWidth", ax_json_retain(named(display, L"width")));
        ax_json_set(display, L"pixelHeight", ax_json_retain(named(display, L"height")));
        snapshot = ax_json_object(); ax_json_set(snapshot, L"app", app); ax_json_set(snapshot, L"display", display);
        ax_json_set(snapshot, L"nodes", output); output = NULL;
        ax_json_set(snapshot, L"roots", roots); roots = NULL;
        ax_json_set(snapshot, L"truncated", ax_json_boolean(uia->work->truncated));
    }
    if (SUCCEEDED(IUIAutomation_GetFocusedElementBuildCache(uia->automation, uia->cache, &focus)) && focus) {
        uint64_t hash = 0;
        BSTR identity = identity_string(uia, focus, &hash);
        record = check_work(uia) ? find_element(uia, focus, identity, hash) : NULL;
        if (record && record->capture == uia->capture) ax_json_set(snapshot, L"focused", ax_json_string(record->id));
        SysFreeString(identity);
    }
    if (!continue_capture(uia)) goto done;
    if (!ax_json_valid(snapshot)) { uia->work->error = E_OUTOFMEMORY; goto done; }
    if (!accessibility_uia_active(uia) || GetForegroundWindow() != window) {
        uia->work->error = HRESULT_FROM_WIN32(ERROR_CANCELLED); goto done;
    }
    if (uia->work->truncated) { uia->work->error = HRESULT_FROM_WIN32(ERROR_MORE_DATA); goto done; }
    for (i = 0; i < count; ++i) {
        record = captured[i];
        ax_json_release(record->metadata); record->metadata = record->captured_metadata; record->captured_metadata = NULL;
        SysFreeString(record->text); record->text = record->captured_text; record->captured_text = NULL;
        record->complete_text = record->captured_complete_text;
    }
    uia->foreground = window; prune_records(uia); success = 1;
done:
    if (queue) { for (i = 0; i < ASB_AX_MAX_NODES; ++i) RELEASE(queue[i].element); free(queue); }
    free(captured); RELEASE(focus); RELEASE(children); RELEASE(expanded); RELEASE(root);
    ax_json_release(output); ax_json_release(roots);
    status = FAILED(work.error) ? work.error : E_FAIL;
    uia->work = previous;
    if (success) { *result = snapshot; return S_OK; }
    ax_json_release(snapshot);
    return status;
}
static Json *query_metadata(AccessibilityUia *uia, UiaRecord *record, BSTR *text, int *complete_text) {
    IUIAutomationElement *expanded = NULL, *parent = NULL, *child = NULL;
    IUIAutomationElementArray *children = NULL;
    UiaRecord scratch = {0};
    Json *result = NULL;
    int count = 0, i;
    *text = NULL; *complete_text = 0;
    if (FAILED(uia->work->error = IUIAutomationElement_BuildUpdatedCache(record->element, uia->cache, &scratch.element)) ||
        !scratch.element) goto done;
    memcpy(scratch.id, record->id, sizeof(scratch.id));
    scratch.pid = record->pid;
    uia->work->text_units = 0;
    result = metadata(uia, &scratch, 1);
    if (!result) goto done;
    if (record->metadata) {
        ax_json_set(result, L"children", ax_json_retain(named(record->metadata, L"children")));
        if (named(record->metadata, L"parent")) ax_json_set(result, L"parent", ax_json_retain(named(record->metadata, L"parent")));
    } else {
        if (SUCCEEDED(IUIAutomationTreeWalker_GetParentElement(uia->walker, record->element, &parent)) && parent) {
            UiaRecord *owner = remember(uia, parent, 0);
            if (owner) { ax_json_set(result, L"parent", ax_json_string(owner->id)); owner->reference = uia->capture; }
        }
        if (SUCCEEDED(IUIAutomationElement_BuildUpdatedCache(record->element, uia->children_cache, &expanded)) && expanded &&
            SUCCEEDED(IUIAutomationElement_GetCachedChildren(expanded, &children)) && children &&
            SUCCEEDED(IUIAutomationElementArray_get_Length(children, &count))) {
            if (count > ASB_AX_MAX_NODES) count = ASB_AX_MAX_NODES;
            for (i = 0; i < count && check_work(uia); ++i) {
                if (SUCCEEDED(IUIAutomationElementArray_GetElement(children, i, &child))) {
                    UiaRecord *item = remember(uia, child, 1);
                    if (item && item != record) { item->reference = uia->capture; ax_json_append(named(result, L"children"), ax_json_string(item->id)); }
                }
                RELEASE(child);
            }
        }
    }
done:
    RELEASE(children); RELEASE(parent); RELEASE(expanded);
    if (!ax_json_valid(result) && SUCCEEDED(uia->work->error)) uia->work->error = E_OUTOFMEMORY;
    RELEASE(scratch.element);
    if (!check_work(uia)) { ax_json_release(result); SysFreeString(scratch.captured_text); return NULL; }
    *text = scratch.captured_text; *complete_text = scratch.captured_complete_text;
    return result;
}
Json *accessibility_uia_request(AccessibilityUia *uia, const Json *message, uint64_t deadline) {
    Json *response = ax_json_object(), *result = NULL, *fresh = NULL;
    const Json *value = named(message, L"value");
    const wchar_t *action = named_text(message, L"action"), *attribute = named_text(message, L"attribute");
    UiaRecord *record = find_record(uia, named_text(message, L"nodeId"));
    HRESULT hr = UIA_E_ELEMENTNOTAVAILABLE;
    BOOL secure = TRUE, enabled = FALSE;
    int pid = 0, mutation = 0, requires_value = 0, fresh_complete_text = 0;
    BSTR fresh_text = NULL;
    IUIAutomationElement *element = NULL;
    IUIAutomationGridPattern *grid = NULL;
    UiaWork work = {0}, *previous = uia->work;
    uia->work = &work; work.deadline = deadline;
    if (!accessibility_uia_active(uia) || !uia->foreground || GetForegroundWindow() != uia->foreground || !record) goto done;
    if (wcslen(action) != ax_json_length(named(message, L"action")) || wcslen(attribute) != ax_json_length(named(message, L"attribute")) ||
        wcslen(named_text(message, L"nodeId")) != ax_json_length(named(message, L"nodeId"))) { hr = E_INVALIDARG; goto done; }
    if (!prepare(uia, deadline)) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); goto done; }
    hr = IUIAutomationElement_get_CurrentProcessId(record->element, &pid);
    if (FAILED(hr) || (DWORD)pid != record->pid) { hr = UIA_E_ELEMENTNOTAVAILABLE; goto done; }
    TRY(IUIAutomationElement_get_CurrentIsPassword(record->element, &secure));
    record->secure = secure != FALSE;
    if (text_equal(action, L"GetBoundingRectangle")) {
        RECT bounds = {0};
        requires_value = 1;
        TRY(IUIAutomationElement_get_CurrentBoundingRectangle(record->element, &bounds));
        result = frame_value(bounds); goto done;
    }
    if (text_equal(action, L"ElementFromPoint")) {
        POINT point;
        UiaRecord *target;
        requires_value = 1;
        if (!decode_point(value, &point)) { hr = E_INVALIDARG; goto done; }
        TRY(IUIAutomation_ElementFromPointBuildCache(uia->automation, point, uia->cache, &element));
        target = remember(uia, element, 1);
        if (target) { target->reference = uia->capture; result = element_reference(target); }
        else hr = UIA_E_ELEMENTNOTAVAILABLE;
        goto done;
    }
    if (text_equal(action, L"GetMetadata")) {
        fresh = query_metadata(uia, record, &fresh_text, &fresh_complete_text);
        if (!fresh) { hr = FAILED(uia->work->error) ? uia->work->error : UIA_E_ELEMENTNOTAVAILABLE; goto done; }
        if (!record->metadata) {
            record->metadata = ax_json_retain(fresh);
            SysFreeString(record->text); record->text = fresh_text; fresh_text = NULL;
            record->complete_text = fresh_complete_text;
        }
        ax_json_set(response, L"record", ax_json_retain(fresh)); hr = S_OK; goto done;
    }
    if (text_equal(action, L"GetProperty")) {
        size_t i;
        VARIANT property;
        requires_value = 1; hr = UIA_E_NOTSUPPORTED;
        for (i = 0; i < ARRAYSIZE(uia_properties); ++i) if (text_equal(attribute, uia_properties[i].name)) break;
        if (i == ARRAYSIZE(uia_properties)) goto done;
        VariantInit(&property);
        hr = IUIAutomationElement_GetCurrentPropertyValue(record->element, *uia_properties[i].id, &property);
        if (SUCCEEDED(hr) && !(secure && (*uia_properties[i].id == UIA_RangeValueValuePropertyId ||
            (*uia_properties[i].id == UIA_ValueValuePropertyId && (property.vt != VT_BSTR || !is_masked_value(property.bstrVal)))))) {
            if (property.vt == VT_BSTR) result = snapshot_string(uia, property.bstrVal ? property.bstrVal : L"", SysStringLen(property.bstrVal));
            else if (property.vt == VT_BOOL) result = ax_json_boolean(property.boolVal != VARIANT_FALSE);
            else if (property.vt == VT_I4) result = ax_json_number(property.lVal);
            else if (property.vt == VT_R8 && isfinite(property.dblVal)) result = ax_json_number(property.dblVal);
        }
        VariantClear(&property);
        if (SUCCEEDED(hr) && !result) hr = UIA_E_NOTSUPPORTED;
        goto done;
    }
    if (text_equal(action, L"Grid.GetItem")) {
        double row, column;
        requires_value = 1; hr = E_INVALIDARG;
        if (ax_json_type(value) != AX_JSON_ARRAY || ax_json_count(value) != 2) goto done;
        row = ax_json_double(ax_json_at(value, 0), NAN); column = ax_json_double(ax_json_at(value, 1), NAN);
        if (!isfinite(column) || !isfinite(row) || row < 0 || column < 0 || row >= INT_MAX || column >= INT_MAX || floor(row) != row || floor(column) != column) goto done;
        if (FAILED(PATTERN(record->element, IUIAutomationGridPattern, UIA_GridPatternId, &grid)) || !grid) { hr = UIA_E_NOTSUPPORTED; goto done; }
        TRY(IUIAutomationGridPattern_GetItem(grid, (int)row, (int)column, &element));
        result = reference(uia, element); goto done;
    }
    if (text_equal(action, L"Text.GetText") || text_equal(action, L"Text.GetBoundingRectangles") ||
        text_equal(action, L"Text.GetAttributeRuns") || text_equal(action, L"Text.RangeFromPoint")) {
        requires_value = 1; hr = text_request(uia, record, action, value, 0, deadline, &result); goto done;
    }
    TRY(IUIAutomationElement_get_CurrentIsEnabled(record->element, &enabled));
    if (!enabled) { hr = UIA_E_ELEMENTNOTENABLED; goto done; }
    mutation = 1;
    if (text_equal(action, L"Text.Select")) hr = text_request(uia, record, action, value, 1, deadline, &result);
    else hr = native_operation(uia, record, action, value, deadline);
done:
    RELEASE(grid); RELEASE(element); ax_json_release(fresh); SysFreeString(fresh_text);
    if (FAILED(uia->work->error)) hr = uia->work->error;
    if (SUCCEEDED(hr) && requires_value && !ax_json_valid(result)) hr = E_OUTOFMEMORY;
    if (FAILED(hr)) { ax_json_release(result); result = NULL; }
    ax_json_set(response, L"ok", ax_json_boolean(SUCCEEDED(hr)));
    ax_json_set(response, L"error", ax_json_number(hr));
    ax_json_set(response, L"mutation", ax_json_boolean(mutation));
    if (result) ax_json_set(response, L"value", result);
    uia->work = previous;
    if (previous && SUCCEEDED(previous->error) && !prepare(uia, previous->deadline))
        previous->error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return response;
}
