/* hcn_parse.c - owned-network identity and HCN document parsing.
 *
 * The pure, offline contract layer of External networking: the
 * deterministic owned-network identity (UUID v5 of the adapter GUID,
 * the AppSandboxExternal-{guid} Name emit/parse), the HCN network
 * properties document parser with its binding-evidence policy machine,
 * the directional binding classification, and the enumerate-result ID
 * array parser. No HCN calls, no WMI, no OS state, no ui_log: this is
 * the only production unit the offline self-check tests link, so its
 * call closure must stay within {this file, json_reader, bcrypt}.
 * Public contracts are declared in hcn_network.h; cross-unit helpers
 * in hcn_private.h. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "json_reader.h"
#include <objbase.h>   /* CLSIDFromString */
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")

/* ---- Helpers ---- */

void guid_to_string(const GUID *g, wchar_t *out, size_t out_len)
{
    swprintf_s(out, out_len,
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ---- External owned-network identity ---- */

/* NT_SUCCESS is a kernel-header macro; user mode does not get it from
   bcrypt.h. Same definition WDM uses (success NTSTATUS values are
   non-negative). */
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

/* UUID v5 namespace for AppSandbox External owned network IDs. The
   namespace GUID is never itself an HCN network ID. */
static const GUID APPSANDBOX_EXTERNAL_NS = {
    0xA7C3E91F, 0x4B62, 0x4D8A,
    { 0x9E, 0x15, 0x2F, 0x6C, 0x8B, 0x0D, 0x47, 0xA1 }
};

/* RFC 4122 UUID byte serialization: Data1/Data2/Data3 big-endian and
   Data4 as stored. A Windows GUID struct's memory layout is not this
   form - never memcpy it as the hash input. */
static void guid_serialize_rfc4122(const GUID *g, unsigned char out[16])
{
    out[0] = (unsigned char)(g->Data1 >> 24);
    out[1] = (unsigned char)(g->Data1 >> 16);
    out[2] = (unsigned char)(g->Data1 >> 8);
    out[3] = (unsigned char)g->Data1;
    out[4] = (unsigned char)(g->Data2 >> 8);
    out[5] = (unsigned char)g->Data2;
    out[6] = (unsigned char)(g->Data3 >> 8);
    out[7] = (unsigned char)g->Data3;
    memcpy(&out[8], g->Data4, 8);
}

/* Derive the stable per-NIC owned External network ID:
   UUID v5(namespace, RFC 4122 bytes of the NIC InterfaceGuid) via
   BCrypt SHA-1. Deterministic per NIC - concurrent creates on the same
   target intentionally compute the same ID. */
HRESULT hcn_external_owned_id(const GUID *nic_interface_guid, GUID *out_id)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char ns_bytes[16];
    unsigned char nic_bytes[16];
    unsigned char digest[20];   /* SHA-1 emits 20 octets; UUID v5 uses 16 */
    GUID result;
    NTSTATUS st;

    if (!nic_interface_guid || !out_id)
        return E_POINTER;

    st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(st))
        return HRESULT_FROM_NT(st);

    st = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    if (!NT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return HRESULT_FROM_NT(st);
    }

    guid_serialize_rfc4122(&APPSANDBOX_EXTERNAL_NS, ns_bytes);
    guid_serialize_rfc4122(nic_interface_guid, nic_bytes);
    st = BCryptHashData(hash, ns_bytes, sizeof(ns_bytes), 0);
    if (NT_SUCCESS(st))
        st = BCryptHashData(hash, nic_bytes, sizeof(nic_bytes), 0);
    if (NT_SUCCESS(st))
        st = BCryptFinishHash(hash, digest, sizeof(digest), 0);

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!NT_SUCCESS(st))
        return HRESULT_FROM_NT(st);

    /* UUID v5: first 16 SHA-1 octets, version 5 and RFC 4122 variant
       bits set, unpacked big-endian into a Windows GUID. */
    digest[6] = (unsigned char)((digest[6] & 0x0F) | 0x50);
    digest[8] = (unsigned char)((digest[8] & 0x3F) | 0x80);
    result.Data1 = ((unsigned long)digest[0] << 24) | ((unsigned long)digest[1] << 16)
                 | ((unsigned long)digest[2] << 8) | digest[3];
    result.Data2 = (unsigned short)(((unsigned short)digest[4] << 8) | digest[5]);
    result.Data3 = (unsigned short)(((unsigned short)digest[6] << 8) | digest[7]);
    memcpy(result.Data4, &digest[8], 8);

    *out_id = result;
    return S_OK;
}

/* Owned network display Name: the literal prefix, '{',
   guid_to_string's 36-char lowercase no-brace GUID, '}' - exactly one
   brace pair. StringFromGUID2's braced uppercase output is never used
   here (it would need conversion and risks double-wrapping). */
HRESULT hcn_external_owned_name(const GUID *nic_interface_guid, wchar_t *out, size_t out_len)
{
    wchar_t guid_str[64];
    int written;

    if (!nic_interface_guid || !out)
        return E_POINTER;

    guid_to_string(nic_interface_guid, guid_str, 64);
    /* _TRUNCATE form: swprintf_s would treat an undersized caller buffer
       as an invalid parameter (fastfail); the graceful failure return is
       required for the caller to detect. */
    written = _snwprintf_s(out, out_len, _TRUNCATE,
                           L"AppSandboxExternal-{%s}", guid_str);
    if (written < 0)
        return E_NOT_SUFFICIENT_BUFFER;
    return S_OK;
}

static int hex_digit_value(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'a' && c <= L'f') return (int)(c - L'a') + 10;
    if (c >= L'A' && c <= L'F') return (int)(c - L'A') + 10;
    return -1;
}

/* Parse the 36-char "8-4-4-4-12" no-brace GUID body. GUID hex letter
   case variants are accepted - the binary result is unchanged. */
static BOOL parse_guid_body(const wchar_t *s, unsigned char out[16])
{
    static const int dash_pos[4] = { 8, 13, 18, 23 };
    int i, bytes = 0, dashes = 0;

    for (i = 0; i < 36; i++) {
        int hi, lo;

        if (dashes < 4 && i == dash_pos[dashes]) {
            if (s[i] != L'-')
                return FALSE;
            dashes++;
            continue;
        }
        /* Hex digits come in pairs; i is a pair start, never odd. */
        if (i + 1 >= 36 || bytes >= 16)
            return FALSE;
        hi = hex_digit_value(s[i]);
        lo = hex_digit_value(s[i + 1]);
        if (hi < 0 || lo < 0)
            return FALSE;
        out[bytes++] = (unsigned char)((hi << 4) | lo);
        i++;
    }
    return (bytes == 16 && dashes == 4);
}

/* Strict owned-Name parse: requires the entire
   "AppSandboxExternal-{<guid>}" format with no suffix; GUID hex case
   variants accepted. Yields only a candidate NIC G - never an
   authorization; callers must still require actual network-ID
   equality before acting on it. */
BOOL hcn_external_parse_owned_name(const wchar_t *name, GUID *out_nic_guid)
{
    static const wchar_t prefix[] = L"AppSandboxExternal-";
    const size_t prefix_len = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    size_t len;
    unsigned char bytes[16];

    if (!name || !out_nic_guid)
        return FALSE;

    len = wcslen(name);
    if (len != prefix_len + 38)   /* '{' + 36-char guid + '}' */
        return FALSE;
    if (wcsncmp(name, prefix, prefix_len) != 0)
        return FALSE;
    if (name[prefix_len] != L'{' || name[len - 1] != L'}')
        return FALSE;
    if (!parse_guid_body(name + prefix_len + 1, bytes))
        return FALSE;

    out_nic_guid->Data1 = ((unsigned long)bytes[0] << 24)
                        | ((unsigned long)bytes[1] << 16)
                        | ((unsigned long)bytes[2] << 8) | bytes[3];
    out_nic_guid->Data2 = (unsigned short)(((unsigned short)bytes[4] << 8) | bytes[5]);
    out_nic_guid->Data3 = (unsigned short)(((unsigned short)bytes[6] << 8) | bytes[7]);
    memcpy(out_nic_guid->Data4, &bytes[8], 8);
    return TRUE;
}

/* Parse a GUID string value (36-char no-brace or 38-char braced;
   case-insensitive) via CLSIDFromString. Identity comparisons remain
 * binary only; textual differences are never identity evidence. */
BOOL parse_guid_value(const wchar_t *s, size_t len, GUID *out)
{
    wchar_t buf[80];

    if (len == 38 && s[0] == L'{' && s[37] == L'}') {
        memcpy(buf, s, len * sizeof(wchar_t));
        buf[38] = L'\0';
    } else if (len == 36) {
        buf[0] = L'{';
        memcpy(buf + 1, s, 36 * sizeof(wchar_t));
        buf[37] = L'}';
        buf[38] = L'\0';
    } else {
        return FALSE;
    }
    return SUCCEEDED(CLSIDFromString(buf, out));
}

HcnAdapterEntry *inventory_entry_by_guid(HcnAdapterEntry *entries,
                                                size_t count, const GUID *guid)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (IsEqualGUID(&entries[i].interface_guid, guid))
            return &entries[i];
    }
    return NULL;
}

/* Parse the HcnEnumerateNetworks result: a JSON array of GUID strings.
   Returns a dynamically allocated GUID array in *out_ids (free() it even
   when the count is 0). S_OK on success, E_FAIL on malformed structure or
   a non-GUID entry, E_OUTOFMEMORY on allocation failure. Truncation is
   never silent: a document that cannot be parsed completely is an error -
   the caller treats it as a FAILED scan (read-layer failure). */
HRESULT parse_hcn_network_id_array(const wchar_t *json,
                                          GUID **out_ids, size_t *out_count)
{
    JsonR r;
    GUID *ids = NULL;
    size_t count = 0;
    size_t cap = 0;

    *out_ids = NULL;
    *out_count = 0;

    json_init(&r, json);
    json_skip_ws(&r);
    if (r.cur >= r.end || *r.cur != L'[')
        return E_FAIL;
    r.cur++;
    json_skip_ws(&r);

    if (r.cur < r.end && *r.cur == L']') {
        r.cur++;
        /* Empty array is a valid, complete result. */
    } else {
        for (;;) {
            wchar_t s[80];
            size_t len;
            BOOL trunc;
            GUID g;

            json_skip_ws(&r);
            if (r.cur >= r.end || *r.cur != L'"')
                return free(ids), E_FAIL;
            if (!json_parse_string(&r, s, ARRAYSIZE(s), &len, &trunc))
                return free(ids), E_FAIL;
            if (trunc || !parse_guid_value(s, len, &g))
                return free(ids), E_FAIL;

            if (count == cap) {
                GUID *grown;
                size_t new_cap = cap ? cap * 2 : 8;
                grown = (GUID *)realloc(ids, new_cap * sizeof(GUID));
                if (!grown)
                    return free(ids), E_OUTOFMEMORY;
                ids = grown;
                cap = new_cap;
            }
            ids[count++] = g;

            json_skip_ws(&r);
            if (r.cur < r.end && *r.cur == L',') { r.cur++; continue; }
            if (r.cur < r.end && *r.cur == L']') { r.cur++; break; }
            return free(ids), E_FAIL;
        }
    }

    json_skip_ws(&r);
    if (r.cur != r.end)
        return free(ids), E_FAIL; /* trailing garbage */

    *out_ids = ids;
    *out_count = count;
    return S_OK;
}

/* ---- HCN network properties document ---- */

static void hcn_props_zero(HcnNetworkProps *props)
{
    ZeroMemory(props, sizeof(*props));
}

/* Record one binding value; capacity limits degrade to explicit flags
   (fail-closed), never silent truncation. */
static void hcn_props_add_binding(HcnNetworkProps *props, BindingSource src,
                                  const wchar_t *value, size_t len, BOOL truncated)
{
    int i;

    if (props->binding_count >= HCN_MAX_BINDING_VALUES) {
        props->binding_overflow = TRUE;
        return;
    }
    if (truncated || len >= HCN_MAX_BINDING_CHARS) {
        /* A value we cannot hold in full cannot be normalized later;
           mark it so classification stays unresolved. */
        props->binding_truncated = TRUE;
        return;
    }
    for (i = 0; i < (int)len; i++)
        props->bindings[props->binding_count][i] = value[i];
    props->bindings[props->binding_count][len] = L'\0';
    props->binding_src[props->binding_count] = src;
    props->binding_count++;
}

/* ---- HCN Policies array parsing ----
 *
 * Each policy object is parsed COMPLETELY into local state before it can
 * contribute any binding value - field order is irrelevant (Settings may
 * precede Type). Only objects whose readable Type is exactly
 * InterfaceConstraint or NetAdapterName contribute a binding value; any
 * other readable Type is ignored entirely (not an occupant, not poison).
 * A policy whose Type is absent or unreadable (truncated, non-string, or
 * conflicting duplicates) is UNRESOLVED: it contributes no placement and
 * forbids off-T placement of the object - it never fails the document
 * and never lands in the ignorable bucket. A recognized binding policy
 * whose payload value is missing, truncated, non-string, or conflicting
 * marks the binding unreadable (unresolved downstream). */

typedef struct {
    BOOL saw_type;
    BOOL type_truncated;
    BOOL type_not_string;    /* Type present but not a JSON string */
    BOOL type_conflict;      /* duplicate Type with a different value */
    BOOL type_is_naa;        /* Type == NetAdapterName (complete string) */
    BOOL type_is_ic;         /* Type == InterfaceConstraint (complete string) */
    wchar_t type_value[HCN_MAX_TYPE_CHARS];

    BOOL saw_settings;
    BOOL settings_conflict;  /* duplicate Settings (malformed) */
    BOOL settings_not_object;/* Settings present but not an object */

    BOOL saw_data;           /* read-side payload tolerance: Data */
    BOOL data_conflict;
    BOOL data_not_object;

    BOOL saw_naa;            /* NetworkAdapterName key inside Settings */
    BOOL naa_truncated;
    BOOL naa_not_string;
    BOOL naa_conflict;       /* duplicate NAA with a different value */
    BOOL naa_from_data;      /* a Data-sourced NAA key: not evidence (the Data tolerance is InterfaceConstraint's) */
    wchar_t naa_value[HCN_MAX_BINDING_CHARS];

    BOOL saw_ic_guid;        /* InterfaceGuid key inside Settings */
    BOOL saw_ic_guid_data;   /* InterfaceGuid key inside Data */
    BOOL ic_guid_truncated;
    BOOL ic_guid_not_string;
    BOOL ic_guid_conflict;   /* duplicate InterfaceGuid within one parent with a different value */
    wchar_t ic_guid_value[HCN_MAX_BINDING_CHARS];       /* Settings-sourced */
    wchar_t ic_guid_data_value[HCN_MAX_BINDING_CHARS];  /* Data-sourced */
} PolicyObjectState;

static void policy_state_zero(PolicyObjectState *st)
{
    ZeroMemory(st, sizeof(*st));
}

/* Record the Type value, detecting conflicting duplicates. The value
   buffer is NUL-terminated when not truncated. */
static void policy_record_type(PolicyObjectState *st,
                               const wchar_t *value, BOOL truncated)
{
    if (st->saw_type) {
        if (st->type_truncated != truncated ||
            (!truncated && _wcsicmp(st->type_value, value) != 0))
            st->type_conflict = TRUE;
        return;
    }
    st->saw_type = TRUE;
    st->type_truncated = truncated;
    st->type_is_naa = (!truncated && _wcsicmp(value, L"NetAdapterName") == 0);
    st->type_is_ic = (!truncated && _wcsicmp(value, L"InterfaceConstraint") == 0);
    if (!truncated)
        wcsncpy_s(st->type_value, HCN_MAX_TYPE_CHARS, value, _TRUNCATE);
}

/* Record the NetworkAdapterName value, detecting conflicting duplicates.
   A Data-sourced NetworkAdapterName is not NAA evidence: the read-side
   Data tolerance is InterfaceConstraint's; an off-location NAA
   makes the slot unreadable instead - fail-closed, never silently
   ignored. */
static void policy_record_naa(PolicyObjectState *st,
                              const wchar_t *value, BOOL truncated,
                              BOOL from_data)
{
    if (from_data) {
        st->naa_from_data = TRUE;
        return;
    }
    if (st->saw_naa) {
        if (st->naa_truncated != truncated ||
            (!truncated && _wcsicmp(st->naa_value, value) != 0))
            st->naa_conflict = TRUE;
        return;
    }
    st->saw_naa = TRUE;
    st->naa_truncated = truncated;
    if (!truncated)
        wcsncpy_s(st->naa_value, HCN_MAX_BINDING_CHARS, value, _TRUNCATE);
}

/* Record the InterfaceGuid payload value, detecting conflicting
   duplicates within one parent. Values from different parents (Settings
   vs Data) are kept SEPARATELY: a cross-parent disagreement is not a
   parse conflict - both definite values flow to the classifier, whose
   directional table decides (disagreeing definite values with one
   resolving to T are CONFLICT). */
static void policy_record_ic_guid(PolicyObjectState *st,
                                  const wchar_t *value, BOOL truncated,
                                  BOOL from_data)
{
    if (from_data) {
        if (st->saw_ic_guid_data) {
            if (st->ic_guid_truncated != truncated ||
                (!truncated && _wcsicmp(st->ic_guid_data_value, value) != 0))
                st->ic_guid_conflict = TRUE;
            return;
        }
        st->saw_ic_guid_data = TRUE;
        st->ic_guid_truncated = st->ic_guid_truncated || truncated;
        if (!truncated)
            wcsncpy_s(st->ic_guid_data_value, HCN_MAX_BINDING_CHARS, value, _TRUNCATE);
        return;
    }
    if (st->saw_ic_guid) {
        if (st->ic_guid_truncated != truncated ||
            (!truncated && _wcsicmp(st->ic_guid_value, value) != 0))
            st->ic_guid_conflict = TRUE;
        return;
    }
    st->saw_ic_guid = TRUE;
    st->ic_guid_truncated = truncated;
    if (!truncated)
        wcsncpy_s(st->ic_guid_value, HCN_MAX_BINDING_CHARS, value, _TRUNCATE);
}

/* Walk one policy payload object (Settings or Data), recording the
   recognized binding keys. from_data marks the Data parent: the
   InterfaceGuid tolerance applies, the NetworkAdapterName one
   does not. Returns FALSE only on structural errors. */
static BOOL policy_walk_payload_object(JsonR *r, PolicyObjectState *st,
                                       BOOL from_data)
{
    json_skip_ws(r);
    if (r->cur >= r->end || *r->cur != L'{')
        return FALSE;
    r->cur++;

    json_skip_ws(r);
    if (r->cur < r->end && *r->cur == L'}') {
        r->cur++;
        return TRUE;
    }

    for (;;) {
        wchar_t skey[64];
        size_t slen;
        BOOL strunc;

        json_skip_ws(r);
        if (r->cur >= r->end || *r->cur != L'"')
            return FALSE;
        if (!json_parse_string(r, skey, ARRAYSIZE(skey), &slen, &strunc))
            return FALSE;
        json_skip_ws(r);
        if (r->cur >= r->end || *r->cur != L':')
            return FALSE;
        r->cur++;

        if (strunc) {
            return FALSE;  /* unidentifiable key */
        } else if (_wcsicmp(skey, L"NetworkAdapterName") == 0) {
            json_skip_ws(r);
            if (r->cur < r->end && *r->cur == L'"') {
                wchar_t v[HCN_MAX_BINDING_CHARS];
                size_t vlen;
                BOOL vtrunc;

                if (!json_parse_string(r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return FALSE;
                policy_record_naa(st, v, vtrunc, from_data);
            } else {
                /* Value present but not a string: skip it structurally
                   and remember it is unusable. */
                st->naa_not_string = TRUE;
                if (!json_skip_value(r))
                    return FALSE;
            }
        } else if (_wcsicmp(skey, L"InterfaceGuid") == 0) {
            json_skip_ws(r);
            if (r->cur < r->end && *r->cur == L'"') {
                wchar_t v[HCN_MAX_BINDING_CHARS];
                size_t vlen;
                BOOL vtrunc;

                if (!json_parse_string(r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return FALSE;
                policy_record_ic_guid(st, v, vtrunc, from_data);
            } else {
                st->ic_guid_not_string = TRUE;
                if (!json_skip_value(r))
                    return FALSE;
            }
        } else {
            if (!json_skip_value(r))
                return FALSE;
        }

        json_skip_ws(r);
        if (r->cur < r->end && *r->cur == L',') { r->cur++; continue; }
        if (r->cur < r->end && *r->cur == L'}') { r->cur++; return TRUE; }
        return FALSE;
    }
}

static BOOL hcn_parse_policies_array(JsonR *r, HcnNetworkProps *props)
{
    json_skip_ws(r);
    if (r->cur >= r->end || *r->cur != L'[')
        return FALSE;
    r->cur++;

    json_skip_ws(r);
    if (r->cur < r->end && *r->cur == L']') {
        r->cur++;
        return TRUE;
    }

    for (;;) {
        wchar_t key[64];
        size_t klen;
        BOOL ktrunc;
        BOOL object_done = FALSE;
        PolicyObjectState st;

        policy_state_zero(&st);

        json_skip_ws(r);
        if (r->cur >= r->end || *r->cur != L'{')
            return FALSE;
        r->cur++;

        json_skip_ws(r);
        if (r->cur < r->end && *r->cur == L'}') {
            r->cur++;
            object_done = TRUE;
        }

        while (!object_done) {
            json_skip_ws(r);
            if (r->cur >= r->end || *r->cur != L'"')
                return FALSE;
            if (!json_parse_string(r, key, ARRAYSIZE(key), &klen, &ktrunc))
                return FALSE;
            json_skip_ws(r);
            if (r->cur >= r->end || *r->cur != L':')
                return FALSE;
            r->cur++;

            if (ktrunc) {
                /* An unidentifiable key: the policy object cannot be
                   classified reliably. */
                return FALSE;
            } else if (_wcsicmp(key, L"Type") == 0) {
                json_skip_ws(r);
                if (r->cur < r->end && *r->cur == L'"') {
                    wchar_t t[HCN_MAX_TYPE_CHARS];
                    size_t tlen;
                    BOOL ttrunc;

                    if (!json_parse_string(r, t, ARRAYSIZE(t), &tlen, &ttrunc))
                        return FALSE;
                    policy_record_type(&st, t, ttrunc);
                } else {
                    /* Type present but unreadable as the string it must
                       be: unresolved, never ignored. */
                    st.type_not_string = TRUE;
                    if (!json_skip_value(r))
                        return FALSE;
                }
            } else if (_wcsicmp(key, L"Settings") == 0) {
                json_skip_ws(r);
                if (r->cur < r->end && *r->cur == L'{') {
                    if (st.saw_settings)
                        st.settings_conflict = TRUE;  /* duplicate Settings */
                    st.saw_settings = TRUE;
                    if (!policy_walk_payload_object(r, &st, FALSE))
                        return FALSE;
                } else {
                    st.settings_not_object = TRUE;
                    if (!json_skip_value(r))
                        return FALSE;
                }
            } else if (_wcsicmp(key, L"Data") == 0) {
                /* Read-side tolerance: a foreign document may carry the
                   policy payload object under Data; the write side
                   never does. */
                json_skip_ws(r);
                if (r->cur < r->end && *r->cur == L'{') {
                    if (st.saw_data)
                        st.data_conflict = TRUE;  /* duplicate Data */
                    st.saw_data = TRUE;
                    if (!policy_walk_payload_object(r, &st, TRUE))
                        return FALSE;
                } else {
                    st.data_not_object = TRUE;
                    if (!json_skip_value(r))
                        return FALSE;
                }
            } else {
                if (!json_skip_value(r))
                    return FALSE;
            }

            json_skip_ws(r);
            if (r->cur < r->end && *r->cur == L',') { r->cur++; continue; }
            if (r->cur < r->end && *r->cur == L'}') { r->cur++; object_done = TRUE; break; }
            return FALSE;
        }

        /* Apply this policy object's rules only now that it is fully
           parsed (field-order independent). */
        if (!st.saw_type || st.type_truncated || st.type_not_string ||
            st.type_conflict) {
            /* Policy with an absent or unreadable Type: unresolved - no
               placement, forbids off-T. Never the ignorable bucket. */
            props->unresolved_type_policy = TRUE;
        } else if (st.type_is_ic) {
            if (st.settings_conflict || st.data_conflict ||
                st.ic_guid_conflict) {
                props->binding_unreadable = TRUE;
            } else if (!st.saw_ic_guid && !st.saw_ic_guid_data) {
                /* InterfaceConstraint policy without a readable payload
                   value (missing, non-object, not a string). */
                props->binding_unreadable = TRUE;
            } else if (st.ic_guid_truncated || st.ic_guid_not_string) {
                props->binding_unreadable = TRUE;
            } else {
                /* Cross-parent disagreement is not a parse conflict:
                   both definite values are evidence for the classifier's
                   directional table. */
                if (st.saw_ic_guid)
                    hcn_props_add_binding(props, BINDING_SRC_IC_POLICY,
                                          st.ic_guid_value,
                                          wcslen(st.ic_guid_value), FALSE);
                if (st.saw_ic_guid_data &&
                    (!st.saw_ic_guid ||
                     _wcsicmp(st.ic_guid_data_value, st.ic_guid_value) != 0))
                    hcn_props_add_binding(props, BINDING_SRC_IC_POLICY,
                                          st.ic_guid_data_value,
                                          wcslen(st.ic_guid_data_value), FALSE);
            }
        } else if (st.type_is_naa) {
            if (st.settings_conflict || st.naa_conflict) {
                props->binding_unreadable = TRUE;
            } else if (!st.saw_settings || !st.saw_naa || st.naa_from_data ||
                       st.naa_truncated || st.naa_not_string ||
                       st.settings_not_object) {
                /* NetAdapterName policy without a unique readable
                   Settings-sourced value (Data is not an accepted NAA
                   location): unresolved, never off-T. */
                props->binding_unreadable = TRUE;
            } else {
                hcn_props_add_binding(props, BINDING_SRC_NAA_POLICY,
                                      st.naa_value, wcslen(st.naa_value),
                                      FALSE);
            }
        }
        /* Type is present, readable, and neither InterfaceConstraint nor
           NetAdapterName: ignored entirely - not an occupant, not poison
           for the object. */

        json_skip_ws(r);
        if (r->cur < r->end && *r->cur == L',') { r->cur++; continue; }
        if (r->cur < r->end && *r->cur == L']') { r->cur++; return TRUE; }
        return FALSE;
    }
}

/* Parse the HcnQueryNetworkProperties output document into props.
   Returns S_OK when the document is structurally valid (required fields
   may still be absent - callers validate); E_FAIL on malformed structure,
   malformed required values, or conflicting duplicate fields. A document
   that fails here degrades that network's scan record to unproven (the
   scan continues); it never fails the whole enumeration scan. */
HRESULT parse_hcn_network_properties(const wchar_t *json,
                                     HcnNetworkProps *props)
{
    JsonR r;
    wchar_t key[64];
    BOOL saw_policies = FALSE;

    hcn_props_zero(props);

    json_init(&r, json);
    json_skip_ws(&r);
    if (r.cur >= r.end || *r.cur != L'{')
        return E_FAIL;
    r.cur++;

    json_skip_ws(&r);
    if (r.cur < r.end && *r.cur == L'}') {
        r.cur++;
    } else {
        for (;;) {
            size_t klen;
            BOOL trunc;

            json_skip_ws(&r);
            if (r.cur >= r.end || *r.cur != L'"')
                return E_FAIL;
            if (!json_parse_string(&r, key, ARRAYSIZE(key), &klen, &trunc))
                return E_FAIL;
            json_skip_ws(&r);
            if (r.cur >= r.end || *r.cur != L':')
                return E_FAIL;
            r.cur++;

            if (trunc) {
                if (!json_skip_value(&r))
                    return E_FAIL;
            } else if (_wcsicmp(key, L"ID") == 0) {
                wchar_t v[80];
                size_t vlen;
                BOOL vtrunc;
                GUID g;

                json_skip_ws(&r);
                if (r.cur >= r.end || *r.cur != L'"')
                    return E_FAIL;
                if (!json_parse_string(&r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return E_FAIL;
                if (vtrunc || !parse_guid_value(v, vlen, &g))
                    return E_FAIL;
                if (props->has_id && !IsEqualGUID(&props->id, &g))
                    return E_FAIL; /* conflicting duplicate ID */
                props->has_id = TRUE;
                props->id = g;
            } else if (_wcsicmp(key, L"Name") == 0) {
                wchar_t v[HCN_MAX_NAME_CHARS];
                size_t vlen;
                BOOL vtrunc;

                json_skip_ws(&r);
                if (r.cur >= r.end || *r.cur != L'"')
                    return E_FAIL;
                if (!json_parse_string(&r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return E_FAIL;
                if (props->has_name &&
                    (props->name_truncated != vtrunc ||
                     (vlen < HCN_MAX_NAME_CHARS &&
                      _wcsicmp(props->name, v) != 0)))
                    return E_FAIL; /* conflicting duplicate Name */
                props->has_name = TRUE;
                props->name_truncated = vtrunc;
                if (!vtrunc)
                    wcscpy_s(props->name, HCN_MAX_NAME_CHARS, v);
            } else if (_wcsicmp(key, L"Type") == 0) {
                wchar_t v[HCN_MAX_TYPE_CHARS];
                size_t vlen;
                BOOL vtrunc;

                json_skip_ws(&r);
                if (r.cur >= r.end || *r.cur != L'"')
                    return E_FAIL;
                if (!json_parse_string(&r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return E_FAIL;
                if (props->has_type &&
                    (props->type_truncated != vtrunc ||
                     (vlen < HCN_MAX_TYPE_CHARS &&
                      _wcsicmp(props->type, v) != 0)))
                    return E_FAIL; /* conflicting duplicate Type */
                props->has_type = TRUE;
                props->type_truncated = vtrunc;
                if (!vtrunc)
                    wcscpy_s(props->type, HCN_MAX_TYPE_CHARS, v);
            } else if (_wcsicmp(key, L"SwitchGuid") == 0) {
                wchar_t v[80];
                size_t vlen;
                BOOL vtrunc;
                GUID g;

                json_skip_ws(&r);
                if (r.cur >= r.end || *r.cur != L'"')
                    return E_FAIL;
                if (!json_parse_string(&r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                    return E_FAIL;
                if (vtrunc || !parse_guid_value(v, vlen, &g))
                    return E_FAIL;
                if (props->has_switch_guid && !IsEqualGUID(&props->switch_guid, &g))
                    return E_FAIL; /* conflicting duplicate SwitchGuid */
                props->has_switch_guid = TRUE;
                props->switch_guid = g;
            } else if (_wcsicmp(key, L"Policies") == 0) {
                if (saw_policies)
                    return E_FAIL;  /* duplicate Policies key: malformed */
                saw_policies = TRUE;
                if (!hcn_parse_policies_array(&r, props))
                    return E_FAIL;
            } else if (_wcsicmp(key, L"InterfaceConstraint") == 0) {
                json_skip_ws(&r);
                if (r.cur < r.end && *r.cur == L'"') {
                    wchar_t v[HCN_MAX_BINDING_CHARS];
                    size_t vlen;
                    BOOL vtrunc;

                    if (!json_parse_string(&r, v, ARRAYSIZE(v), &vlen, &vtrunc))
                        return E_FAIL;
                    hcn_props_add_binding(props, BINDING_SRC_TOP_IC, v, vlen,
                                          vtrunc);
                } else if (r.cur < r.end && *r.cur == L'{') {
                    /* Object form (the measured HNS echo shape): walk the
                       members with the same tolerance as the policy
                       payload. A string InterfaceGuid member is the
                       top-level binding evidence; unknown members are
                       skipped structurally (the unknown-document-field
                       rule). Anything unreadable - no member, non-string
                       member, conflicting duplicates - keeps the opaque
                       fail-closed marker. */
                    BOOL saw_guid = FALSE;
                    BOOL guid_conflict = FALSE;
                    BOOL guid_truncated = FALSE;
                    BOOL guid_not_string = FALSE;
                    wchar_t gv[HCN_MAX_BINDING_CHARS];

                    r.cur++;
                    json_skip_ws(&r);
                    if (r.cur < r.end && *r.cur == L'}') {
                        r.cur++;
                    } else {
                        for (;;) {
                            wchar_t mkey[64];
                            size_t mlen;
                            BOOL mtrunc;

                            json_skip_ws(&r);
                            if (r.cur >= r.end || *r.cur != L'"')
                                return E_FAIL;
                            if (!json_parse_string(&r, mkey, ARRAYSIZE(mkey),
                                                   &mlen, &mtrunc))
                                return E_FAIL;
                            json_skip_ws(&r);
                            if (r.cur >= r.end || *r.cur != L':')
                                return E_FAIL;
                            r.cur++;

                            if (_wcsicmp(mkey, L"InterfaceGuid") == 0) {
                                json_skip_ws(&r);
                                if (r.cur < r.end && *r.cur == L'"') {
                                    wchar_t v[HCN_MAX_BINDING_CHARS];
                                    size_t vlen;
                                    BOOL vtrunc;

                                    if (!json_parse_string(&r, v, ARRAYSIZE(v),
                                                           &vlen, &vtrunc))
                                        return E_FAIL;
                                    if (saw_guid) {
                                        if (guid_truncated != vtrunc ||
                                            (!vtrunc && _wcsicmp(gv, v) != 0))
                                            guid_conflict = TRUE;
                                    } else {
                                        saw_guid = TRUE;
                                        guid_truncated = vtrunc;
                                        if (!vtrunc)
                                            wcscpy_s(gv, HCN_MAX_BINDING_CHARS, v);
                                    }
                                } else {
                                    guid_not_string = TRUE;
                                    if (!json_skip_value(&r))
                                        return E_FAIL;
                                }
                            } else {
                                if (!json_skip_value(&r))
                                    return E_FAIL;
                            }

                            json_skip_ws(&r);
                            if (r.cur < r.end && *r.cur == L',') { r.cur++; continue; }
                            if (r.cur < r.end && *r.cur == L'}') { r.cur++; break; }
                            return E_FAIL;
                        }
                    }
                    if (saw_guid && !guid_conflict && !guid_truncated &&
                        !guid_not_string) {
                        hcn_props_add_binding(props, BINDING_SRC_TOP_IC, gv,
                                              wcslen(gv), FALSE);
                    } else {
                        props->binding_non_string = TRUE;
                    }
                } else {
                    /* Unknown shape: recorded as opaque (never parsed as
                       a binding and never ignored as a decision). */
                    props->binding_non_string = TRUE;
                    if (!json_skip_value(&r))
                        return E_FAIL;
                }
            } else {
                /* Unknown field (e.g. LayeredOn, an HNS-internal GUID):
                   skipped structurally - not binding evidence (measured
                   build-26200 shape). */
                if (!json_skip_value(&r))
                    return E_FAIL;
            }

            json_skip_ws(&r);
            if (r.cur < r.end && *r.cur == L',') { r.cur++; continue; }
            if (r.cur < r.end && *r.cur == L'}') { r.cur++; break; }
            return E_FAIL;
        }
    }

    json_skip_ws(&r);
    if (r.cur != r.end)
        return E_FAIL; /* trailing garbage */

    return S_OK;
}

/* ---- Binding normalization and directional classification ---- */

/* Resolve one decoded binding value against the target GUID.
   GUID-typed evidence (InterfaceConstraint fields, top or policy
   payload) compares directly: an unequal valid GUID is off-T even when
   absent from current L0. NetAdapterName values count as GUID
   evidence only when the GUID equals the target or matches a current
   interface; otherwise they translate as names through unique
   FriendlyName, then unique Description. Ambiguous or failed
   translation is unresolved; never first-match. */
static void normalize_binding_value(const wchar_t *value, BindingSource src,
                                    const HcnAdapterInventory *inv,
                                    const GUID *target_guid,
                                    BindingResolution *out)
{
    const HcnAdapterEntry *match = NULL;
    int matches = 0;
    size_t i;
    GUID g;
    BOOL guid_shaped = FALSE;

    out->match = BINDING_UNRESOLVED;
    out->resolved = FALSE;
    ZeroMemory(&g, sizeof(g));

    if (!value || !value[0])
        return;

    /* 1. GUID-shaped form. */
    {
        size_t len = wcslen(value);
        if (len == 36 || (len == 38 && value[0] == L'{')) {
            if (parse_guid_value(value, len, &g))
                guid_shaped = TRUE;
        }
    }

    if (src == BINDING_SRC_IC_POLICY || src == BINDING_SRC_TOP_IC) {
        /* Typed GUID field: compare directly with T. */
        if (guid_shaped) {
            out->resolved = TRUE;
            out->resolved_guid = g;
            out->match = IsEqualGUID(&g, target_guid)
                         ? BINDING_MATCH : BINDING_MISMATCH;
        }
        /* A non-GUID value in a typed GUID field is malformed:
           unresolved. */
        return;
    }

    /* NetAdapterName value: GUID-shaped counts as GUID evidence only
       when it equals T or a current interface; a GUID-shaped connection
       NAME that matches nothing falls through to name translation. */
    if (guid_shaped) {
        if (IsEqualGUID(&g, target_guid)) {
            out->resolved = TRUE;
            out->resolved_guid = g;
            out->match = BINDING_MATCH;
            return;
        }
        if (inv && inventory_entry_by_guid(inv->entries, inv->count, &g)) {
            out->resolved = TRUE;
            out->resolved_guid = g;
            out->match = BINDING_MISMATCH;
            return;
        }
    }

    /* 2. FriendlyName form (case-insensitive, unique). */
    if (inv) {
        for (i = 0; i < inv->count; i++) {
            const HcnAdapterEntry *e = &inv->entries[i];
            if (e->friendly_name[0] && _wcsicmp(e->friendly_name, value) == 0) {
                match = e;
                matches++;
            }
        }
        /* 3. Interface Description form (case-insensitive, unique). */
        if (matches == 0) {
            for (i = 0; i < inv->count; i++) {
                const HcnAdapterEntry *e = &inv->entries[i];
                if (e->description[0] && _wcsicmp(e->description, value) == 0) {
                    match = e;
                    matches++;
                }
            }
        }
    }

    if (matches != 1) {
        /* Zero or ambiguous resolution: unresolved. */
        return;
    }

    out->resolved = TRUE;
    out->resolved_guid = match->interface_guid;
    out->match = IsEqualGUID(&match->interface_guid, target_guid)
                 ? BINDING_MATCH : BINDING_MISMATCH;
}

/* Aggregate every binding value of one network into the directional
 * classification relative to target_guid. On-T is sticky (a definite
 * value resolving to T stands whatever else failed to read); off-T
 * requires every value definite, all agreeing on one other GUID, and no
 * unresolved marker; disagreeing definite values with any resolving to T
 * are conflict; everything else is unproven. */
BindingClass classify_binding_values(const HcnNetworkProps *props,
                                     const HcnAdapterInventory *inv,
                                     const GUID *target_guid)
{
    BOOL has_match = FALSE;
    BOOL has_mismatch = FALSE;
    BOOL have_definite = FALSE;
    BOOL all_off_agree = TRUE;
    BOOL have_off_guid = FALSE;
    GUID off_guid;
    BOOL unresolved;
    int i;

    ZeroMemory(&off_guid, sizeof(off_guid));

    unresolved = props->binding_unreadable || props->binding_truncated ||
                 props->binding_non_string || props->unresolved_type_policy;

    /* Object-level incompleteness: a partial read set is not positive
       placement evidence. */
    if (props->binding_overflow)
        return BINDING_UNPROVEN;

    for (i = 0; i < props->binding_count; i++) {
        BindingResolution r;

        normalize_binding_value(props->bindings[i], props->binding_src[i],
                                inv, target_guid, &r);
        if (r.match == BINDING_UNRESOLVED) {
            unresolved = TRUE;
            continue;
        }
        have_definite = TRUE;
        if (r.match == BINDING_MATCH) {
            has_match = TRUE;
        } else {
            has_mismatch = TRUE;
            if (!have_off_guid) {
                have_off_guid = TRUE;
                off_guid = r.resolved_guid;
            } else if (!IsEqualGUID(&off_guid, &r.resolved_guid)) {
                all_off_agree = FALSE;
            }
        }
    }

    if (has_match && has_mismatch)
        return BINDING_CONFLICT;
    if (has_match)
        return BINDING_ON_T;
    if (has_mismatch && all_off_agree && !unresolved)
        return BINDING_OFF_T;
    return BINDING_UNPROVEN;
}
