#include "accessibility_uia.h"
#include "../transport/asb_transport.h"
#include "../transport/accessibility_protocol.h"
#include <ole2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* UIA error from winerror.h; avoid UIAutomationClient.h's C module definitions. */
#ifndef UIA_E_ELEMENTNOTAVAILABLE
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#endif
static volatile LONG stopping;
static BOOL WINAPI stop_handler(DWORD event) { (void)event; InterlockedExchange(&stopping, 1); return TRUE; }
static int is_stopping(void) { return InterlockedCompareExchange(&stopping, 0, 0) != 0; }
static int read_bytes(AsbConn *connection, void *buffer, size_t size) {
    unsigned char *bytes = buffer;
    uint64_t deadline = GetTickCount64() + ASB_AX_REQUEST_TIMEOUT_MS;
    while (size && !is_stopping()) {
        int ready = asb_poll(connection, 100), count;
        if (ready < 0 || GetTickCount64() >= deadline) return 0;
        if (!ready) continue;
        count = asb_recv(connection, bytes, (int)size);
        if (count <= 0) return 0;
        bytes += count; size -= (size_t)count;
    }
    return size == 0;
}
static AccessibilityJson *read_message(AsbConn *connection) {
    unsigned char header[4]; size_t size; char *bytes; AccessibilityJson *message;
    if (!read_bytes(connection, header, sizeof(header))) return NULL;
    size = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) | ((size_t)header[2] << 8) | header[3];
    if (!size || size > ASB_AX_MAX_MESSAGE) return NULL;
    bytes = malloc(size); if (!bytes) return NULL;
    if (!read_bytes(connection, bytes, size)) { free(bytes); return NULL; }
    message = ax_json_parse(bytes, size); free(bytes);
    if (ax_json_type(message) != AX_JSON_OBJECT) { ax_json_release(message); return NULL; }
    return message;
}
static int write_bytes(AsbConn *connection, const void *buffer, size_t size) {
    const char *bytes = buffer;
    while (size && !is_stopping()) {
        int count = asb_send(connection, bytes, (int)size);
        if (count <= 0) return 0;
        bytes += count; size -= (size_t)count;
    }
    return size == 0;
}
static int write_message(AsbConn *connection, const AccessibilityJson *message) {
    size_t size = 0; char *bytes = ax_json_serialize(message, &size); unsigned char header[4]; int result;
    if (!bytes || !size || size > ASB_AX_MAX_MESSAGE) { free(bytes); return 0; }
    header[0] = (unsigned char)(size >> 24); header[1] = (unsigned char)(size >> 16);
    header[2] = (unsigned char)(size >> 8); header[3] = (unsigned char)size;
    result = write_bytes(connection, header, sizeof(header)) && write_bytes(connection, bytes, size);
    free(bytes); return result;
}
typedef struct PendingAction { AccessibilityJson *message; uint64_t prepared_at; } PendingAction;
typedef struct AccessibilitySession {
    AccessibilityUia *uia;
    AsbConn *connection;
    wchar_t session[37];
    PendingAction pending[ASB_AX_MAX_PENDING_REQUESTS];
    size_t pending_count;
    uint64_t revision, refresh_generation, work_generation, observed_change, capture_after, background_capture_after;
    HWND observed_foreground;
    AccessibilityJson *deferred_message;
    unsigned capture_failures;
    int subscribed, capture_requested, background_capture_requested, capturing;
} AccessibilitySession;
static void clear_pending(AccessibilitySession *s) {
    size_t i; for (i = 0; i < s->pending_count; ++i) ax_json_release(s->pending[i].message);
    s->pending_count = 0;
}
static PendingAction take_pending(AccessibilitySession *s, size_t index) {
    PendingAction item = s->pending[index];
    memmove(s->pending + index, s->pending + index + 1, (--s->pending_count - index) * sizeof(*s->pending));
    return item;
}
static AccessibilityJson *message_base(AccessibilitySession *s, const wchar_t *type) {
    AccessibilityJson *message = ax_json_object();
    ax_json_set(message, L"type", ax_json_string(type));
    ax_json_set(message, L"version", ax_json_number(ASB_AX_VERSION));
    ax_json_set(message, L"session", ax_json_string(s->session));
    return message;
}
static int status_message(AccessibilitySession *s, const wchar_t *status, int invalidated) {
    AccessibilityJson *message = message_base(s, L"status"); int result;
    ax_json_set(message, L"status", ax_json_string(status));
    ax_json_set(message, L"refreshSupported", ax_json_boolean(1));
    ax_json_set(message, L"navigationQueriesSupported", ax_json_boolean(1));
    ax_json_set(message, L"actionConfirmationSupported", ax_json_boolean(1));
    ax_json_set(message, L"invalidated", ax_json_boolean(invalidated));
    result = write_message(s->connection, message); ax_json_release(message); return result;
}
/* Consumes result. Request remains borrowed throughout execution. */
static int reply(AccessibilitySession *s, const AccessibilityJson *request, AccessibilityJson *result) {
    int sent;
    ax_json_set(result, L"type", ax_json_string(L"actionResult"));
    ax_json_set(result, L"version", ax_json_number(ASB_AX_VERSION));
    ax_json_set(result, L"session", ax_json_string(s->session));
    ax_json_set(result, L"requestId", ax_json_retain(ax_json_get(request, L"requestId")));
    if (ax_json_bool(ax_json_get(result, L"mutation"), 0)) {
        ++s->work_generation;
        s->capture_failures = 0;
        /* The host follows a mutation with an explicit refresh. Match Mac:
         * do not race that refresh with an extra speculative capture. */
        s->capture_requested = 0; s->background_capture_requested = 0;
    }
    ax_json_remove(result, L"mutation");
    sent = write_message(s->connection, result); ax_json_release(result); return sent;
}
static int reject(AccessibilitySession *s, const AccessibilityJson *request, HRESULT error) {
    AccessibilityJson *result = ax_json_object();
    ax_json_set(result, L"ok", ax_json_boolean(0));
    ax_json_set(result, L"error", ax_json_number(error));
    return reply(s, request, result);
}
static int execute(AccessibilitySession *s, const AccessibilityJson *message, uint64_t deadline) {
    AccessibilityJson *result;
    if (!s->subscribed || !ax_json_equal_text(ax_json_get(message, L"session"), s->session))
        return reject(s, message, UIA_E_ELEMENTNOTAVAILABLE);
    if (GetTickCount64() >= deadline) return reject(s, message, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    result = accessibility_uia_request(s->uia, message, deadline);
    if (!result) return reject(s, message, E_OUTOFMEMORY);
    return reply(s, message, result);
}
static int handle(AccessibilitySession *s, const AccessibilityJson *message) {
    const AccessibilityJson *type = ax_json_get(message, L"type"), *request;
    size_t i, request_length;
    double number;
    if (ax_json_double(ax_json_get(message, L"version"), 0) != ASB_AX_VERSION) return 0;
    if (ax_json_equal_text(type, L"subscribe")) {
        s->subscribed = ax_json_bool(ax_json_get(message, L"enabled"), 0);
        clear_pending(s); accessibility_uia_reset(s->uia);
        ++s->work_generation;
        s->capture_failures = 0; s->observed_foreground = NULL;
        s->background_capture_requested = 0;
        s->capture_requested = s->subscribed; s->capture_after = GetTickCount64() + ASB_AX_INPUT_SETTLE_MS; return 1;
    }
    if (ax_json_equal_text(type, L"refresh")) {
        uint64_t requested;
        number = ax_json_double(ax_json_get(message, L"generation"), -1);
        if (!isfinite(number) || number < 0 || number > 9007199254740991.0 || floor(number) != number) return 0;
        requested = (uint64_t)number;
        if (requested < s->refresh_generation || (requested == s->refresh_generation && (s->capture_requested || s->capturing))) return 1;
        ++s->work_generation;
        s->capture_failures = 0;
        s->background_capture_requested = 0;
        s->refresh_generation = requested; s->capture_requested = s->subscribed; s->capture_after = GetTickCount64(); return 1;
    }
    request = ax_json_get(message, L"requestId"); request_length = ax_json_length(request);
    if (!request_length || request_length > 128 || wmemchr(ax_json_text(request), 0, request_length)) return 0;
    if (ax_json_equal_text(type, L"action")) {
        AccessibilityJson *ready; int sent;
        if (!ax_json_equal_text(ax_json_get(message, L"session"), s->session)) return reject(s, message, UIA_E_ELEMENTNOTAVAILABLE);
        if (!ax_json_bool(ax_json_get(message, L"confirmBeforeExecution"), 0))
            return execute(s, message, GetTickCount64() + ASB_AX_REQUEST_TIMEOUT_MS);
        for (i = 0; i < s->pending_count; ++i)
            if (ax_json_equal_text(ax_json_get(s->pending[i].message, L"requestId"), ax_json_text(request))) return 0;
        if (s->pending_count >= ASB_AX_MAX_PENDING_REQUESTS) return reject(s, message, HRESULT_FROM_WIN32(ERROR_BUSY));
        s->pending[s->pending_count].message = ax_json_retain((AccessibilityJson *)message);
        s->pending[s->pending_count++].prepared_at = GetTickCount64();
        ready = message_base(s, L"actionReady");
        ax_json_set(ready, L"requestId", ax_json_retain((AccessibilityJson *)request));
        sent = write_message(s->connection, ready); ax_json_release(ready); return sent;
    }
    if (ax_json_equal_text(type, L"actionCommit")) {
        PendingAction pending; int sent;
        number = ax_json_double(ax_json_get(message, L"remainingMs"), -1);
        if (!ax_json_equal_text(ax_json_get(message, L"session"), s->session) || !isfinite(number) || number < 0 || number > ASB_AX_REQUEST_TIMEOUT_MS) return 0;
        for (i = 0; i < s->pending_count; ++i)
            if (ax_json_equal_text(ax_json_get(s->pending[i].message, L"requestId"), ax_json_text(request))) break;
        if (i == s->pending_count) return 1;
        pending = take_pending(s, i);
        /* Charge confirmation round trip to original budget, including suspension. */
        sent = execute(s, pending.message, pending.prepared_at + (uint64_t)number);
        ax_json_release(pending.message); return sent;
    }
    return 0;
}
static int expire_pending(AccessibilitySession *s) {
    size_t i = 0;
    uint64_t now = GetTickCount64();
    while (i < s->pending_count) {
        PendingAction pending;
        int sent;
        if (now - s->pending[i].prepared_at < ASB_AX_REQUEST_TIMEOUT_MS) { ++i; continue; }
        pending = take_pending(s, i);
        sent = reject(s, pending.message, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        ax_json_release(pending.message); if (!sent) return 0;
    }
    return 1;
}
static void observe_changes(AccessibilitySession *s) {
    uint64_t change = accessibility_uia_change_generation(s->uia), now = GetTickCount64();
    HWND foreground = GetForegroundWindow();
    if (foreground != s->observed_foreground) {
        s->observed_foreground = foreground; s->observed_change = change;
        ++s->work_generation; s->capture_failures = 0;
        s->capture_requested = 1; s->background_capture_requested = 0; s->capture_after = now;
    } else if (change != s->observed_change) {
        uint64_t next = accessibility_uia_last_change_time(s->uia) + 100;
        s->observed_change = change;
        if (next < s->background_capture_after) next = s->background_capture_after;
        /* As on Mac, semantic notifications during capture queue background
         * work without cancelling it or delaying an explicit refresh. */
        if (!s->capture_requested) {
            if (!s->capturing) ++s->work_generation;
            s->capture_requested = 1; s->background_capture_requested = 1; s->capture_after = next;
        } else if (s->background_capture_requested && next < s->capture_after) s->capture_after = next;
    }
}
typedef struct CaptureContext { AccessibilitySession *session; uint64_t generation, started, tick; int connected; } CaptureContext;
static int capture_continue(void *context) {
    CaptureContext *c = context;
    AccessibilitySession *s = c->session;
    size_t count;
    observe_changes(s);
    c->connected = c->connected && expire_pending(s);
    if (is_stopping() || !c->connected || !s->subscribed || s->work_generation != c->generation ||
        GetTickCount64() - c->started >= ASB_AX_CAPTURE_TIMEOUT_MS) return 0;
    if (GetTickCount64() >= c->tick) {
        c->connected = status_message(s, L"updating", 0);
        c->tick = GetTickCount64() + 500;
    }
    /* Match the Mac collector's serial request pump. Reading the tree must
     * not repeatedly throw away a capture that is still in progress. */
    for (count = 0; c->connected && count < ASB_AX_MAX_PENDING_REQUESTS; ++count) {
        AccessibilityJson *message;
        int ready = asb_poll(s->connection, 0);
        if (ready < 0) { c->connected = 0; break; }
        if (!ready) break;
        message = read_message(s->connection);
        if (!message) { c->connected = 0; break; }
        if (ax_json_equal_text(ax_json_get(message, L"type"), L"subscribe")) {
            /* Reset frees records, so process it only after traversal unwinds. */
            s->deferred_message = message; return 0;
        }
        c->connected = handle(s, message); ax_json_release(message);
        if (!s->subscribed || s->work_generation != c->generation ||
            GetTickCount64() - c->started >= ASB_AX_CAPTURE_TIMEOUT_MS) return 0;
    }
    return c->connected;
}
static void run_session(AccessibilityUia *uia, AsbConn *connection) {
    AccessibilitySession s = {0}; GUID guid; wchar_t uuid[40]; uint64_t heartbeat; int active;
    s.uia = uia; s.connection = connection;
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, uuid, ARRAYSIZE(uuid))) return;
    memcpy(s.session, uuid + 1, 36 * sizeof(wchar_t));
    accessibility_uia_reset(uia); asb_set_timeout(connection, 1000, 1000);
    if (!status_message(&s, L"ready", 0)) goto done;
    heartbeat = GetTickCount64() + 1000; active = accessibility_uia_active(uia);
    while (!is_stopping()) {
        uint64_t now = GetTickCount64(); int ready, current_active;
        if (!expire_pending(&s)) break;
        if (s.deferred_message) {
            AccessibilityJson *message = s.deferred_message; s.deferred_message = NULL;
            int handled = handle(&s, message); ax_json_release(message);
            if (!handled) break;
            continue;
        }
        ready = asb_poll(connection, 50); if (ready < 0) break;
        if (ready) {
            AccessibilityJson *message = read_message(connection);
            int handled = message && handle(&s, message);
            ax_json_release(message); if (!handled) break; continue;
        }
        current_active = accessibility_uia_active(uia);
        if (active != current_active) {
            active = current_active; accessibility_uia_reset(uia);
            ++s.work_generation; s.background_capture_requested = 0;
            s.capture_failures = 0; s.observed_foreground = NULL;
            s.capture_requested = active && s.subscribed; s.capture_after = now;
            if (!status_message(&s, active ? L"updating" : L"inactive", 1)) break;
        }
        if (s.subscribed && active) {
            HWND foreground = GetForegroundWindow();
            observe_changes(&s);
            if (s.capture_requested && now >= s.capture_after && !s.pending_count) {
                CaptureContext context; AccessibilityJson *snapshot = NULL; HRESULT capture_result;
                uint64_t finished, background_delay;
                if (!status_message(&s, L"updating", accessibility_uia_foreground_changed(uia))) break;
                context.session = &s; context.started = GetTickCount64(); context.tick = context.started + 500; context.connected = 1;
                context.generation = s.work_generation;
                s.capture_requested = 0; s.background_capture_requested = 0; s.capturing = 1;
                capture_result = accessibility_uia_capture(uia, context.started + ASB_AX_CAPTURE_TIMEOUT_MS, capture_continue, &context, &snapshot);
                s.capturing = 0;
                if (!context.connected) { ax_json_release(snapshot); break; }
                if (s.work_generation != context.generation || !s.subscribed ||
                    capture_result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                    ax_json_release(snapshot);
                } else if (SUCCEEDED(capture_result)) {
                    ax_json_set(snapshot, L"type", ax_json_string(L"snapshot"));
                    ax_json_set(snapshot, L"version", ax_json_number(ASB_AX_VERSION));
                    ax_json_set(snapshot, L"session", ax_json_string(s.session));
                    ax_json_set(snapshot, L"revision", ax_json_number((double)++s.revision));
                    ax_json_set(snapshot, L"refreshGeneration", ax_json_number((double)s.refresh_generation));
                    ax_json_set(snapshot, L"captureMs", ax_json_number((double)(GetTickCount64() - context.started)));
                    ready = write_message(connection, snapshot); ax_json_release(snapshot); if (!ready) break;
                    s.capture_failures = 0;
                } else if (!accessibility_uia_active(uia) || GetForegroundWindow() != foreground) {
                    s.capture_requested = s.subscribed; s.background_capture_requested = 0;
                    s.capture_after = GetTickCount64();
                } else {
                    if (!status_message(&s, L"captureFailed", 0)) break;
                    if (s.capture_failures < 3) ++s.capture_failures;
                    /* Match the Mac collector: retry transient failures twice,
                     * then wait for new input or a semantic change. */
                    if (capture_result != HRESULT_FROM_WIN32(ERROR_MORE_DATA) && s.capture_failures <= 2) {
                        s.capture_requested = 1; s.background_capture_requested = 0;
                        s.capture_after = GetTickCount64() + 500 * s.capture_failures;
                    }
                }
                finished = GetTickCount64(); background_delay = 3 * (finished - context.started);
                s.background_capture_after = finished + (background_delay < 1000 ? 1000 : background_delay);
                if (s.capture_requested && s.background_capture_requested && s.capture_after < s.background_capture_after)
                    s.capture_after = s.background_capture_after;
                heartbeat = GetTickCount64() + 1000;
            }
        }
        if (GetTickCount64() >= heartbeat) {
            if (!status_message(&s, active ? L"ready" : L"inactive", 0)) break;
            heartbeat = GetTickCount64() + 1000;
        }
    }
done:
    ax_json_release(s.deferred_message); clear_pending(&s); accessibility_uia_reset(uia);
}
int wmain(void) {
    AccessibilityUia *uia = NULL; HRESULT hr; int result = 0;
    SetConsoleCtrlHandler(stop_handler, TRUE);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!ax_json_initialize()) { fwprintf(stderr, L"Accessibility JSON initialization failed\n"); return 1; }
    if (asb_transport_init() != 0) { ax_json_uninitialize(); return 1; }
    hr = accessibility_uia_create(&uia);
    if (FAILED(hr)) { fwprintf(stderr, L"Accessibility initialization failed: 0x%08lx\n", (unsigned long)hr); result = 1; goto done; }
    while (!is_stopping()) {
        AsbListener *listener = asb_listen(ASB_CH_ACCESSIBILITY);
        if (!listener) { Sleep(1000); continue; }
        while (!is_stopping()) {
            AsbConn *connection = asb_accept(listener, 333);
            if (!connection) continue;
            run_session(uia, connection); asb_close(connection);
        }
        asb_close_listener(listener);
    }
done:
    accessibility_uia_destroy(uia); ax_json_uninitialize(); return result;
}
