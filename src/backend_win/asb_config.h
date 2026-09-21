#ifndef ASB_CONFIG_H
#define ASB_CONFIG_H

/* asb_config.h - the vms.cfg stream interface, extracted from asb_core.c
 * so the offline test binary can drive the REAL loader and saver.
 *
 * The stream functions are pure CRT stream I/O on a caller-owned FILE*:
 * they never open, close, or seek it (the open helpers below are the
 * only file-opening surface, shared by the core wrapper and the
 * fixtures). No logging, no HCS, no VMMS, no asb_core.h - the caps are
 * EXPLICIT parameters, never ASB_MAX_VMS/ASB_MAX_TEMPLATES.
 *
 * unique_id has no channel here on purpose: it is assigned by the core
 * wrapper from the one allocator shared with asb_vm_create, so the
 * identity space stays single-sourced. */

#include <stdio.h>
#include "hcs_vm.h"

/* Open vms.cfg for reading with the baseline's BOM sniff: UTF-8-BOM and
   UTF-16LE-BOM files reopen as "r,ccs=UTF-8", anything else (ANSI) as
   "r". NULL on failure - a caller that mirrors the baseline treats
   NULL as "nothing to load", never as a stream error. */
FILE *asb_config_open_read(const wchar_t *path);

/* Open vms.cfg for writing ("w,ccs=UTF-8": BOM + CRLF canonical form).
   NULL on failure. */
FILE *asb_config_open_write(const wchar_t *path);

/* Parse the whole stream into the caller's arrays. vm_count and
   template_count are IN/OUT: rows are APPENDED at the current counts
   and existing rows are never touched. The VM cap STOPS the read (every
   later block in the file is dropped); the template cap SKIPS the
   block and keeps parsing - two different baseline behaviors, moved
   verbatim. The whole-stream BOOL folds in one ferror() probe after
   the read; a stream error still leaves the rows parsed so far in
   place. A single VM's invalid selector is that VM's state, never a
   stream-level FALSE. */
BOOL asb_load_vm_list_stream(FILE *f, VmInstance *vms, int vm_cap,
                             int *vm_count, TemplateInfo *templates,
                             int template_cap, int *template_count,
                             wchar_t *last_iso_path, size_t last_iso_cap,
                             BOOL *suppress_tray_warn);

/* Write the whole stream (BOM + CRLF + canonical key order; the
   InternalSwitch pair sits immediately after NetworkMode=, ahead of
   every unvalidated string key). The whole-stream BOOL folds in one
   ferror() probe after the write. */
BOOL asb_save_vm_list_stream(FILE *f, const VmInstance *vms, int vm_count,
                             const TemplateInfo *templates, int template_count,
                             const wchar_t *last_iso_path,
                             BOOL suppress_tray_warn);

/* The storable-name rule for the internal vSwitch selector: length
   within the cap plus the shared character class (asb_net_limits.h).
   len is the caller's view of the value's length (wcslen at the
   entrances, SysStringLen in the WMI layer). */
BOOL asb_internal_switch_value_valid(const wchar_t *name, size_t len);

#endif /* ASB_CONFIG_H */
