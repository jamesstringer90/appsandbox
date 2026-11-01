#include "json_reader.h"

#include <wchar.h>

#define JSON_MAX_DEPTH 24

void json_init(JsonR *r, const wchar_t *text)
{
    if (!text)
        text = L"";
    r->cur = text;
    r->end = text + wcslen(text);
    r->depth = 0;
}

void json_skip_ws(JsonR *r)
{
    while (r->cur < r->end) {
        wchar_t c = *r->cur;
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r')
            r->cur++;
        else
            break;
    }
}

/* Read exactly 4 hex digits (for \uXXXX). */
static BOOL json_read_hex4(JsonR *r, unsigned int *out)
{
    unsigned int v = 0;
    int i;

    if (r->end - r->cur < 4)
        return FALSE;
    for (i = 0; i < 4; i++) {
        wchar_t c = r->cur[i];
        unsigned int d;
        if (c >= L'0' && c <= L'9')      d = (unsigned int)(c - L'0');
        else if (c >= L'a' && c <= L'f') d = (unsigned int)(c - L'a') + 10;
        else if (c >= L'A' && c <= L'F') d = (unsigned int)(c - L'A') + 10;
        else return FALSE;
        v = (v << 4) | d;
    }
    r->cur += 4;
    *out = v;
    return TRUE;
}

BOOL json_parse_string(JsonR *r, wchar_t *out, size_t cap,
                       size_t *out_len, BOOL *truncated)
{
    size_t len = 0;

    *out_len = 0;
    *truncated = FALSE;

    if (r->cur >= r->end || *r->cur != L'"')
        return FALSE;
    r->cur++;

    for (;;) {
        wchar_t c;

        if (r->cur >= r->end)
            return FALSE; /* unterminated */
        c = *r->cur;

        if (c == L'"') {
            r->cur++;
            if (out && cap > 0)
                out[len < cap ? len : cap - 1] = L'\0';
            *out_len = len;
            return TRUE;
        }

        if (c != L'\\') {
            if ((unsigned int)c < 0x20)
                return FALSE; /* raw control character */
            r->cur++;
        } else {
            wchar_t e;

            r->cur++;
            if (r->cur >= r->end)
                return FALSE;
            e = *r->cur;
            r->cur++;
            switch (e) {
            case L'"':  c = L'"';  break;
            case L'\\': c = L'\\'; break;
            case L'/':  c = L'/';  break;
            case L'b':  c = L'\b'; break;
            case L'f':  c = L'\f'; break;
            case L'n':  c = L'\n'; break;
            case L'r':  c = L'\r'; break;
            case L't':  c = L'\t'; break;
            case L'u': {
                unsigned int u1, u2;

                if (!json_read_hex4(r, &u1))
                    return FALSE;
                if (u1 >= 0xD800 && u1 <= 0xDBFF) {
                    /* High surrogate: must be followed by \uDC00-\uDFFF. */
                    if (r->end - r->cur >= 2 &&
                        r->cur[0] == L'\\' && r->cur[1] == L'u') {
                        r->cur += 2;
                        if (!json_read_hex4(r, &u2))
                            return FALSE;
                        if (!(u2 >= 0xDC00 && u2 <= 0xDFFF))
                            return FALSE; /* malformed surrogate pair */
                        if (out && cap > 1) {
                            if (len + 1 < cap) {
                                out[len] = (wchar_t)u1;
                                out[len + 1] = (wchar_t)u2;
                            } else {
                                *truncated = TRUE;
                            }
                        } else if (out) {
                            *truncated = TRUE;
                        }
                        len += 2;
                        continue;
                    }
                    return FALSE; /* unpaired high surrogate */
                }
                if (u1 >= 0xDC00 && u1 <= 0xDFFF)
                    return FALSE; /* unpaired low surrogate */
                c = (wchar_t)u1;
                break;
            }
            default:
                return FALSE; /* unknown escape */
            }
        }

        if (out && cap > 0) {
            if (len < cap - 1)
                out[len] = c;
            else
                *truncated = TRUE;
        }
        len++;
    }
}

BOOL json_skip_value(JsonR *r)
{
    wchar_t c;

    json_skip_ws(r);
    if (r->cur >= r->end)
        return FALSE;
    c = *r->cur;

    if (c == L'"') {
        size_t len;
        BOOL trunc;
        return json_parse_string(r, NULL, 0, &len, &trunc);
    }

    if (c == L'{') {
        r->cur++;
        if (++r->depth > JSON_MAX_DEPTH) { r->depth--; return FALSE; }
        json_skip_ws(r);
        if (r->cur < r->end && *r->cur == L'}') { r->cur++; r->depth--; return TRUE; }
        for (;;) {
            size_t klen;
            BOOL trunc;

            json_skip_ws(r);
            if (r->cur >= r->end || *r->cur != L'"') { r->depth--; return FALSE; }
            if (!json_parse_string(r, NULL, 0, &klen, &trunc)) { r->depth--; return FALSE; }
            json_skip_ws(r);
            if (r->cur >= r->end || *r->cur != L':') { r->depth--; return FALSE; }
            r->cur++;
            if (!json_skip_value(r)) { r->depth--; return FALSE; }
            json_skip_ws(r);
            if (r->cur < r->end && *r->cur == L',') { r->cur++; continue; }
            if (r->cur < r->end && *r->cur == L'}') { r->cur++; r->depth--; return TRUE; }
            r->depth--;
            return FALSE;
        }
    }

    if (c == L'[') {
        r->cur++;
        if (++r->depth > JSON_MAX_DEPTH) { r->depth--; return FALSE; }
        json_skip_ws(r);
        if (r->cur < r->end && *r->cur == L']') { r->cur++; r->depth--; return TRUE; }
        for (;;) {
            if (!json_skip_value(r)) { r->depth--; return FALSE; }
            json_skip_ws(r);
            if (r->cur < r->end && *r->cur == L',') { r->cur++; continue; }
            if (r->cur < r->end && *r->cur == L']') { r->cur++; r->depth--; return TRUE; }
            r->depth--;
            return FALSE;
        }
    }

    /* number / true / false / null */
    if (c == L't') {
        if (r->end - r->cur < 4 || wcsncmp(r->cur, L"true", 4) != 0) return FALSE;
        r->cur += 4;
        return TRUE;
    }
    if (c == L'f') {
        if (r->end - r->cur < 5 || wcsncmp(r->cur, L"false", 5) != 0) return FALSE;
        r->cur += 5;
        return TRUE;
    }
    if (c == L'n') {
        if (r->end - r->cur < 4 || wcsncmp(r->cur, L"null", 4) != 0) return FALSE;
        r->cur += 4;
        return TRUE;
    }
    if (c == L'-' || (c >= L'0' && c <= L'9')) {
        const wchar_t *start = r->cur;
        r->cur++;
        while (r->cur < r->end) {
            wchar_t d = *r->cur;
            if ((d >= L'0' && d <= L'9') || d == L'.' || d == L'e' ||
                d == L'E' || d == L'+' || d == L'-')
                r->cur++;
            else
                break;
        }
        return r->cur > start;
    }
    return FALSE;
}
