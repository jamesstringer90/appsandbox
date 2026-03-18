#ifndef WEBVIEW2_BRIDGE_H
#define WEBVIEW2_BRIDGE_H

#include <windows.h>

/* Initialize WebView2 in the given parent window.
   Returns TRUE if initialization started (async completion). */
BOOL webview2_init(HWND parent, HINSTANCE hInstance);

/* Post a JSON message to the WebView2 frontend.
   Safe to call before WebView2 is ready (messages are queued). */
void webview2_post(const wchar_t *json);

/* Flush queued messages. Call when JS signals it is ready. */
void webview2_flush_queue(void);

/* Resize the WebView2 to fill the parent's client area. */
void webview2_resize(HWND parent);

/* Clean up WebView2 resources. */
void webview2_cleanup(void);

/* Check if WebView2 is ready to receive messages. */
BOOL webview2_is_ready(void);

/* Set callback for incoming messages from the JS frontend. */
typedef void (*WebView2MessageCallback)(const wchar_t *json);
void webview2_set_message_callback(WebView2MessageCallback cb);

/* ---- JSON builder helpers ---- */

typedef struct {
    wchar_t *buf;
    size_t   cap;
    size_t   len;
    int      count;  /* number of key-value pairs at current level */
} JsonBuilder;

void jb_init(JsonBuilder *jb, wchar_t *buf, size_t cap);
void jb_object_begin(JsonBuilder *jb);
void jb_object_end(JsonBuilder *jb);
void jb_array_begin(JsonBuilder *jb, const wchar_t *key);
void jb_array_end(JsonBuilder *jb);
void jb_string(JsonBuilder *jb, const wchar_t *key, const wchar_t *val);
void jb_int(JsonBuilder *jb, const wchar_t *key, int val);
void jb_bool(JsonBuilder *jb, const wchar_t *key, BOOL val);

/* Low-level append helpers (for building raw JSON arrays, etc.) */
void jb_append(JsonBuilder *jb, const wchar_t *s);
void jb_append_escaped(JsonBuilder *jb, const wchar_t *s);

/* ---- Simple JSON parser (for messages from JS) ---- */

/* Extract a string value for a given key from a JSON string.
   Returns TRUE if found. out is null-terminated. */
BOOL json_get_string(const wchar_t *json, const wchar_t *key,
                     wchar_t *out, size_t out_len);

/* Extract an integer value for a given key. Returns TRUE if found. */
BOOL json_get_int(const wchar_t *json, const wchar_t *key, int *out);

/* Extract a boolean value for a given key. Returns TRUE if found. */
BOOL json_get_bool(const wchar_t *json, const wchar_t *key, BOOL *out);

#endif /* WEBVIEW2_BRIDGE_H */
