#ifndef ASB_CLIPBOARD_TEMP_H
#define ASB_CLIPBOARD_TEMP_H

#include <stdint.h>

#ifdef _WIN32
#include <wchar.h>
typedef wchar_t AsbClipboardPathChar;
#else
#include <sys/types.h>
typedef char AsbClipboardPathChar;
#endif

typedef struct AsbClipboardTemp AsbClipboardTemp;

AsbClipboardTemp *asb_clipboard_temp_create(const AsbClipboardPathChar *parent);
const AsbClipboardPathChar *asb_clipboard_temp_path(const AsbClipboardTemp *temp);
#ifdef _WIN32
int asb_clipboard_temp_prepare(AsbClipboardTemp *temp);
#endif
void asb_clipboard_temp_publish(AsbClipboardTemp *temp, const void *owner, uint64_t token);
void asb_clipboard_temp_changed(uint64_t token);
void asb_clipboard_temp_retire_owner(const void *owner);
void asb_clipboard_temp_discard(AsbClipboardTemp *temp);
void asb_clipboard_temp_collect(const AsbClipboardPathChar *parent);
void asb_clipboard_temp_shutdown(void);

#ifndef _WIN32
int asb_clipboard_temp_set_owner(AsbClipboardTemp *temp, uid_t uid, gid_t gid);
int asb_clipboard_temp_directory(AsbClipboardTemp *temp, const char *relative);
int asb_clipboard_temp_file(AsbClipboardTemp *temp, const char *relative);
int asb_clipboard_temp_symlink(AsbClipboardTemp *temp, const char *relative, const char *target);
#endif

#endif
