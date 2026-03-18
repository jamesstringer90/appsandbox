#ifndef PREREQ_H
#define PREREQ_H

#include <windows.h>

/* Check if a Windows optional feature is enabled (via DISM). */
BOOL prereq_is_feature_enabled(const wchar_t *feature_name);

/* Enable a Windows optional feature (via DISM). Sets *reboot_required on 3010. */
BOOL prereq_enable_feature(const wchar_t *feature_name, BOOL *reboot_required);

/* Check all HCS prerequisites and auto-enable if missing.
   Returns TRUE if HCS is ready to use. */
BOOL prereq_check_all(void);

#endif /* PREREQ_H */
