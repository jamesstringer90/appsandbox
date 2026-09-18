#ifndef JSON_READER_H
#define JSON_READER_H

#include <stddef.h>
#include <windows.h>

/* UTF-16 JSON reader used by the Windows HCN path.
   Not a project-wide JSON library: it walks the grammar so callers can
   decode selected fields and skip the rest structurally. macOS does not
   use this file. */

typedef struct {
    const wchar_t *cur;
    const wchar_t *end;
    int depth;
} JsonR;

void json_init(JsonR *r, const wchar_t *text);
void json_skip_ws(JsonR *r);

/* Parse a JSON string (cursor on the opening quote), decoding standard
   escapes and \uXXXX (surrogate pairs validated and preserved as UTF-16).
   Decoded content is stored into out[0..cap-1] when out != NULL; when the
   value does not fit, *truncated is set (the full logical length is still
   reported in *out_len). out == NULL only validates/skips. Returns FALSE
   on malformed escapes, unpaired surrogates or an unterminated string. */
BOOL json_parse_string(JsonR *r, wchar_t *out, size_t cap,
                       size_t *out_len, BOOL *truncated);

/* Structurally skip one JSON value of any type (object, array, string,
   number, bool, null). Returns FALSE on malformed input. */
BOOL json_skip_value(JsonR *r);

#endif /* JSON_READER_H */
