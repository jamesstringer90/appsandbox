#ifndef ASB_ACCESSIBILITY_JSON_H
#define ASB_ACCESSIBILITY_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct AccessibilityJson AccessibilityJson;
typedef enum AccessibilityJsonType {
    AX_JSON_NULL, AX_JSON_BOOLEAN, AX_JSON_NUMBER, AX_JSON_STRING,
    AX_JSON_ARRAY, AX_JSON_OBJECT
} AccessibilityJsonType;

/* Constructors return an owned reference. Getters return borrowed values.
 * Set/append consume their value even on failure. Retain explicitly when
 * sharing a value with another container or the last published snapshot. */
AccessibilityJson *ax_json_null(void);
AccessibilityJson *ax_json_boolean(int value);
AccessibilityJson *ax_json_number(double value);
AccessibilityJson *ax_json_string(const wchar_t *value);
AccessibilityJson *ax_json_string_n(const wchar_t *value, size_t length);
AccessibilityJson *ax_json_array(void);
AccessibilityJson *ax_json_object(void);
AccessibilityJson *ax_json_retain(AccessibilityJson *value);
void ax_json_release(AccessibilityJson *value);
int ax_json_set(AccessibilityJson *object, const wchar_t *key, AccessibilityJson *value);
int ax_json_append(AccessibilityJson *array, AccessibilityJson *value);
void ax_json_remove(AccessibilityJson *object, const wchar_t *key);
AccessibilityJson *ax_json_get(const AccessibilityJson *object, const wchar_t *key);
AccessibilityJson *ax_json_at(const AccessibilityJson *container, size_t index);
const wchar_t *ax_json_key(const AccessibilityJson *object, size_t index);
size_t ax_json_count(const AccessibilityJson *container);
AccessibilityJsonType ax_json_type(const AccessibilityJson *value);
const wchar_t *ax_json_text(const AccessibilityJson *value);
size_t ax_json_length(const AccessibilityJson *value);
int ax_json_equal_text(const AccessibilityJson *value, const wchar_t *text);
double ax_json_double(const AccessibilityJson *value, double fallback);
int ax_json_bool(const AccessibilityJson *value, int fallback);
int ax_json_valid(const AccessibilityJson *value);

/* Uses Windows' JSON parser/serializer through the C ABI. UTF-8 buffers use
 * malloc/free. A failed allocation or invalid subtree cannot be serialized. */
int ax_json_initialize(void);
void ax_json_uninitialize(void);
AccessibilityJson *ax_json_parse(const char *utf8, size_t length);
char *ax_json_serialize(const AccessibilityJson *value, size_t *length);

#endif
