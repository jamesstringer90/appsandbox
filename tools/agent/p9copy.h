#ifndef P9COPY_H
#define P9COPY_H

#include <winsock2.h>
#include <windows.h>

/* Result codes */
#define P9_OK        0
#define P9_ERR_CONN  1   /* Connection failed */
#define P9_ERR_PROTO 2   /* Protocol error */
#define P9_ERR_IO    3   /* File I/O error */

/* Logging callback — agent provides its own logger */
typedef void (*P9LogFn)(const char *fmt, ...);

/* Set the log function used by p9copy. If not set, no logging occurs. */
void p9_set_log(P9LogFn fn);

/* Copy a single Plan9 share to a local directory via HvSocket.
   Connects to the host via AF_HYPERV on the given port, attaches to share_name,
   and recursively copies all files to local_dir.
   If filter is non-NULL/non-empty, only copies the named files (semicolon-separated)
   from the share root.
   Skips files that already exist with matching size.
   Returns P9_OK on success, or a P9_ERR_* code.
   files_copied receives the count of files actually written (may be NULL). */
int p9_copy_share(UINT32 port, const char *share_name,
                  const wchar_t *local_dir, const char *filter,
                  int *files_copied);

#endif /* P9COPY_H */
