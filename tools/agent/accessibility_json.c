#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.data.json.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "accessibility_json.h"
#include "../transport/accessibility_protocol.h"

/* ABI IIDs from SDK windows.data.json.h; C declarations have no uuid.lib definitions. */
static const IID iid_json_statics = {0x5f6b544a,0x2f53,0x48e1,{0x91,0xa3,0xf7,0x8b,0x50,0xa6,0x34,0x5c}};
static const IID iid_json_map = {0xc9d9a725,0x786b,0x5113,{0xb4,0xb7,0x9b,0x61,0x76,0x4c,0x22,0x0b}};
static const IID iid_json_vector = {0xd44662bc,0xdce3,0x59a8,{0x92,0x72,0x4b,0x21,0x0f,0x33,0x90,0x8b}};
static const IID iid_json_iterable = {0xdfabb6e1,0x0411,0x5a8f,{0xaa,0x87,0x35,0x4e,0x71,0x10,0xf0,0x99}};

#define MAX_DEPTH 128u
typedef __x_ABI_CWindows_CData_CJson_CIJsonValue NativeValue;
typedef __x_ABI_CWindows_CData_CJson_CIJsonValueStatics NativeStatics;
typedef __x_ABI_CWindows_CData_CJson_CIJsonArray NativeArray;
typedef __x_ABI_CWindows_CData_CJson_CIJsonObject NativeObject;
typedef __FIVector_1_Windows__CData__CJson__CIJsonValue NativeVector;
typedef __FIMap_2_HSTRING_Windows__CData__CJson__CIJsonValue NativeMap;
typedef __FIIterable_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue NativeIterable;
typedef __FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue NativeIterator;
typedef __FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue NativePair;
struct AccessibilityJson {
    size_t refs, count, capacity, length;
    AccessibilityJsonType type;
    int invalid;
    double number;
    wchar_t *text;
    struct JsonEntry { wchar_t *key; AccessibilityJson *value; } *entries;
};
/* Collector-owned MTA state; initialize/uninitialize on that same thread. */
static __declspec(thread) NativeStatics *statics;
static __declspec(thread) int ro_initialized;
#define RELEASE(p) do { if (p) (p)->lpVtbl->Release(p); } while (0)
static AccessibilityJson *make(AccessibilityJsonType type) {
    AccessibilityJson *v = calloc(1, sizeof(*v));
    if (v) { v->refs = 1; v->type = type; }
    return v;
}
AccessibilityJson *ax_json_null(void) { return make(AX_JSON_NULL); }
AccessibilityJson *ax_json_boolean(int b) { AccessibilityJson *v = make(AX_JSON_BOOLEAN); if (v) v->number = !!b; return v; }
AccessibilityJson *ax_json_number(double n) { AccessibilityJson *v; if (!isfinite(n)) return NULL; v = make(AX_JSON_NUMBER); if (v) v->number = n; return v; }
AccessibilityJson *ax_json_string_n(const wchar_t *s, size_t n) {
    AccessibilityJson *v;
    if ((!s && n) || n > ASB_AX_MAX_MESSAGE / sizeof(wchar_t)) return NULL;
    v = make(AX_JSON_STRING); if (!v) return NULL;
    v->text = malloc((n + 1) * sizeof(wchar_t));
    if (!v->text) { free(v); return NULL; }
    if (n) memcpy(v->text, s, n * sizeof(wchar_t));
    v->text[n] = 0; v->length = n; return v;
}
AccessibilityJson *ax_json_string(const wchar_t *s) { return ax_json_string_n(s, s ? wcslen(s) : 0); }
AccessibilityJson *ax_json_array(void) { return make(AX_JSON_ARRAY); }
AccessibilityJson *ax_json_object(void) { return make(AX_JSON_OBJECT); }
AccessibilityJson *ax_json_retain(AccessibilityJson *v) { if (v) ++v->refs; return v; }
void ax_json_release(AccessibilityJson *v) {
    size_t i; if (!v || --v->refs) return;
    for (i = 0; i < v->count; ++i) { free(v->entries[i].key); ax_json_release(v->entries[i].value); }
    free(v->entries); free(v->text); free(v);
}
static int valid(const AccessibilityJson *v, const AccessibilityJson *forbidden, unsigned depth) {
    size_t i;
    if (!v || v->invalid || v == forbidden || depth > MAX_DEPTH) return 0;
    for (i = 0; i < v->count; ++i) if (!valid(v->entries[i].value, forbidden, depth + 1)) return 0;
    return 1;
}
int ax_json_valid(const AccessibilityJson *v) { return valid(v, NULL, 0); }
static int reserve(AccessibilityJson *v) {
    size_t cap; void *p;
    if (v->count < v->capacity) return 1;
    cap = v->capacity ? v->capacity * 2 : 8;
    if (cap > ASB_AX_MAX_MESSAGE / sizeof(*v->entries)) return 0;
    p = realloc(v->entries, cap * sizeof(*v->entries));
    if (!p) return 0;
    v->entries = p; v->capacity = cap; return 1;
}
int ax_json_set(AccessibilityJson *o, const wchar_t *key, AccessibilityJson *v) {
    size_t i; wchar_t *copy;
    if (!o || o->type != AX_JSON_OBJECT || !key || !valid(v, o, 0)) goto fail;
    for (i = 0; i < o->count; ++i) if (!wcscmp(o->entries[i].key, key)) {
        ax_json_release(o->entries[i].value); o->entries[i].value = v; return 1;
    }
    if (!reserve(o)) goto fail;
    copy = _wcsdup(key); if (!copy) goto fail;
    o->entries[o->count].key = copy; o->entries[o->count++].value = v; return 1;
fail:
    if (o) o->invalid = 1;
    ax_json_release(v); return 0;
}
int ax_json_append(AccessibilityJson *a, AccessibilityJson *v) {
    if (!a || a->type != AX_JSON_ARRAY || !valid(v, a, 0) || !reserve(a)) {
        if (a) a->invalid = 1; ax_json_release(v); return 0;
    }
    a->entries[a->count].key = NULL; a->entries[a->count++].value = v; return 1;
}
void ax_json_remove(AccessibilityJson *o, const wchar_t *key) {
    size_t i; if (!o || o->type != AX_JSON_OBJECT || !key) return;
    for (i = 0; i < o->count; ++i) if (!wcscmp(o->entries[i].key, key)) {
        free(o->entries[i].key); ax_json_release(o->entries[i].value);
        memmove(o->entries + i, o->entries + i + 1, (--o->count - i) * sizeof(*o->entries)); return;
    }
}
AccessibilityJson *ax_json_get(const AccessibilityJson *o, const wchar_t *key) {
    size_t i; if (!o || o->type != AX_JSON_OBJECT || !key) return NULL;
    for (i = 0; i < o->count; ++i) if (!wcscmp(o->entries[i].key, key)) return o->entries[i].value;
    return NULL;
}
AccessibilityJson *ax_json_at(const AccessibilityJson *v, size_t i) { return v && i < v->count ? v->entries[i].value : NULL; }
const wchar_t *ax_json_key(const AccessibilityJson *v, size_t i) { return v && v->type == AX_JSON_OBJECT && i < v->count ? v->entries[i].key : NULL; }
size_t ax_json_count(const AccessibilityJson *v) { return v ? v->count : 0; }
AccessibilityJsonType ax_json_type(const AccessibilityJson *v) { return v ? v->type : AX_JSON_NULL; }
const wchar_t *ax_json_text(const AccessibilityJson *v) { return v && v->type == AX_JSON_STRING ? v->text : L""; }
size_t ax_json_length(const AccessibilityJson *v) { return v && v->type == AX_JSON_STRING ? v->length : 0; }
int ax_json_equal_text(const AccessibilityJson *v, const wchar_t *s) { size_t n; if (!v || v->type != AX_JSON_STRING || !s) return 0; n = wcslen(s); return n == v->length && !memcmp(v->text, s, n * sizeof(wchar_t)); }
double ax_json_double(const AccessibilityJson *v, double fallback) { return v && v->type == AX_JSON_NUMBER ? v->number : fallback; }
int ax_json_bool(const AccessibilityJson *v, int fallback) { return v && v->type == AX_JSON_BOOLEAN ? v->number != 0 : fallback; }
int ax_json_initialize(void) {
    HSTRING name = NULL; HRESULT hr;
    if (statics) return 1;
    hr = RoInitialize(RO_INIT_MULTITHREADED); if (FAILED(hr)) return 0;
    ro_initialized = 1;
    hr = WindowsCreateString(L"Windows.Data.Json.JsonValue", 27, &name);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, &iid_json_statics, (void **)&statics);
    WindowsDeleteString(name);
    if (FAILED(hr)) { ax_json_uninitialize(); return 0; }
    return 1;
}
void ax_json_uninitialize(void) { RELEASE(statics); statics = NULL; if (ro_initialized) RoUninitialize(); ro_initialized = 0; }
static HRESULT native_literal(const wchar_t *s, NativeValue **out) {
    HSTRING text = NULL; HRESULT hr = WindowsCreateString(s, (UINT32)wcslen(s), &text);
    if (SUCCEEDED(hr)) hr = statics->lpVtbl->Parse(statics, text, out);
    WindowsDeleteString(text); return hr;
}
static AccessibilityJson *from_native(NativeValue *n, unsigned depth) {
    enum __x_ABI_CWindows_CData_CJson_CJsonValueType type;
    AccessibilityJson *v = NULL, *child = NULL;
    NativeValue *item = NULL;
    NativeArray *array_value = NULL; NativeObject *object_value = NULL;
    NativeVector *array = NULL;
    NativeIterable *iterable = NULL;
    NativeIterator *iterator = NULL;
    NativePair *pair = NULL;
    HSTRING text = NULL, key = NULL;
    UINT32 count = 0, i, length;
    const wchar_t *raw;
    boolean b = 0, more = 0;
    double number;
    HRESULT hr;
    if (depth > MAX_DEPTH) return NULL;
#define TRY(call) do { hr = (call); if (FAILED(hr)) goto fail; } while (0)
    TRY(n->lpVtbl->get_ValueType(n, &type));
    switch ((int)type) {
    case 0: v = ax_json_null(); break;
    case 1: TRY(n->lpVtbl->GetBoolean(n, &b)); v = ax_json_boolean(b); break;
    case 2: TRY(n->lpVtbl->GetNumber(n, &number)); v = ax_json_number(number); break;
    case 3:
        TRY(n->lpVtbl->GetString(n, &text)); raw = WindowsGetStringRawBuffer(text, &length);
        v = ax_json_string_n(raw, length); break;
    case 4:
        v = ax_json_array(); if (!v) goto fail;
        TRY(n->lpVtbl->GetArray(n, &array_value));
        TRY(array_value->lpVtbl->QueryInterface(array_value, &iid_json_vector, (void **)&array));
        TRY(array->lpVtbl->get_Size(array, &count));
        for (i = 0; i < count; ++i) {
            TRY(array->lpVtbl->GetAt(array, i, &item)); child = from_native(item, depth + 1); RELEASE(item); item = NULL;
            if (!ax_json_append(v, child)) goto fail;
        }
        break;
    case 5:
        v = ax_json_object(); if (!v) goto fail;
        TRY(n->lpVtbl->GetObject(n, &object_value));
        TRY(object_value->lpVtbl->QueryInterface(object_value, &iid_json_iterable, (void **)&iterable));
        TRY(iterable->lpVtbl->First(iterable, &iterator));
        TRY(iterator->lpVtbl->get_HasCurrent(iterator, &more));
        while (more) {
            TRY(iterator->lpVtbl->get_Current(iterator, &pair));
            TRY(pair->lpVtbl->get_Key(pair, &key)); raw = WindowsGetStringRawBuffer(key, &length);
            if (wmemchr(raw, 0, length)) goto fail;
            TRY(pair->lpVtbl->get_Value(pair, &item)); child = from_native(item, depth + 1); RELEASE(item); item = NULL;
            if (!ax_json_set(v, raw, child)) goto fail;
            WindowsDeleteString(key); key = NULL; RELEASE(pair); pair = NULL;
            TRY(iterator->lpVtbl->MoveNext(iterator, &more));
        }
        break;
    default: goto fail;
    }
    goto done;
fail:
    ax_json_release(v); v = NULL;
done:
    WindowsDeleteString(text); WindowsDeleteString(key);
    RELEASE(item); RELEASE(pair); RELEASE(iterator); RELEASE(iterable); RELEASE(array); RELEASE(array_value); RELEASE(object_value);
    return v;
#undef TRY
}
static NativeValue *to_native(const AccessibilityJson *v, unsigned depth) {
    NativeValue *n = NULL, *child = NULL;
    NativeArray *array_value = NULL; NativeObject *object_value = NULL;
    NativeVector *array = NULL; NativeMap *object = NULL;
    HSTRING text = NULL; HRESULT hr; size_t i; boolean replaced;
    if (!v || v->invalid || depth > MAX_DEPTH) return NULL;
#define TRY(call) do { hr = (call); if (FAILED(hr)) goto fail; } while (0)
    switch (v->type) {
    case AX_JSON_NULL: TRY(native_literal(L"null", &n)); break;
    case AX_JSON_BOOLEAN: TRY(statics->lpVtbl->CreateBooleanValue(statics, (boolean)(v->number != 0), &n)); break;
    case AX_JSON_NUMBER: TRY(statics->lpVtbl->CreateNumberValue(statics, v->number, &n)); break;
    case AX_JSON_STRING:
        TRY(WindowsCreateString(v->text, (UINT32)v->length, &text));
        TRY(statics->lpVtbl->CreateStringValue(statics, text, &n)); break;
    case AX_JSON_ARRAY:
        TRY(native_literal(L"[]", &n));
        TRY(n->lpVtbl->GetArray(n, &array_value));
        TRY(array_value->lpVtbl->QueryInterface(array_value, &iid_json_vector, (void **)&array));
        for (i = 0; i < v->count; ++i) {
            child = to_native(v->entries[i].value, depth + 1); if (!child) goto fail;
            TRY(array->lpVtbl->Append(array, child)); RELEASE(child); child = NULL;
        }
        break;
    case AX_JSON_OBJECT:
        TRY(native_literal(L"{}", &n));
        TRY(n->lpVtbl->GetObject(n, &object_value));
        TRY(object_value->lpVtbl->QueryInterface(object_value, &iid_json_map, (void **)&object));
        for (i = 0; i < v->count; ++i) {
            child = to_native(v->entries[i].value, depth + 1); if (!child) goto fail;
            TRY(WindowsCreateString(v->entries[i].key, (UINT32)wcslen(v->entries[i].key), &text));
            TRY(object->lpVtbl->Insert(object, text, child, &replaced));
            WindowsDeleteString(text); text = NULL; RELEASE(child); child = NULL;
        }
        break;
    default: goto fail;
    }
    goto done;
fail:
    RELEASE(n); n = NULL;
done:
    WindowsDeleteString(text); RELEASE(child); RELEASE(array); RELEASE(array_value); RELEASE(object_value); RELEASE(object); return n;
#undef TRY
}
/* Structural preflight bounds native parser recursion, not JSON syntax parsing.
 * The platform parser remains authoritative for escapes/tokens/grammar. */
static int bounded_depth(const char *s, size_t length) {
    size_t i; unsigned depth = 0; int quoted = 0, escaped = 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (quoted) { if (escaped) escaped = 0; else if (c == '\\') escaped = 1; else if (c == '"') quoted = 0; }
        else if (c == '"') quoted = 1;
        else if (c == '[' || c == '{') { if (++depth > MAX_DEPTH) return 0; }
        else if (c == ']' || c == '}') { if (!depth) return 0; --depth; }
    }
    return !depth && !quoted;
}
AccessibilityJson *ax_json_parse(const char *utf8, size_t length) {
    wchar_t *wide = NULL; int count; HSTRING text = NULL; NativeValue *native = NULL; AccessibilityJson *v = NULL;
    if (!statics || !utf8 || !length || length > ASB_AX_MAX_MESSAGE || !bounded_depth(utf8, length)) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)length, NULL, 0);
    if (!count) return NULL;
    wide = malloc((size_t)count * sizeof(wchar_t)); if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)length, wide, count)) goto done;
    if (FAILED(WindowsCreateString(wide, (UINT32)count, &text))) goto done;
    if (FAILED(statics->lpVtbl->Parse(statics, text, &native))) goto done;
    v = from_native(native, 0);
done:
    RELEASE(native); WindowsDeleteString(text); free(wide); return v;
}
char *ax_json_serialize(const AccessibilityJson *v, size_t *length) {
    NativeValue *native = NULL; HSTRING text = NULL; UINT32 count; const wchar_t *wide; int bytes; char *out = NULL;
    if (length) *length = 0;
    if (!statics || !ax_json_valid(v)) return NULL;
    native = to_native(v, 0); if (!native) return NULL;
    if (FAILED(native->lpVtbl->Stringify(native, &text))) goto done;
    wide = WindowsGetStringRawBuffer(text, &count);
    if (count > ASB_AX_MAX_MESSAGE) goto done;
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)count, NULL, 0, NULL, NULL);
    if (bytes <= 0 || (unsigned)bytes > ASB_AX_MAX_MESSAGE) goto done;
    out = malloc((size_t)bytes + 1); if (!out) goto done;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)count, out, bytes, NULL, NULL)) { free(out); out = NULL; goto done; }
    out[bytes] = 0; if (length) *length = (size_t)bytes;
done:
    WindowsDeleteString(text); RELEASE(native); return out;
}
