#ifndef PREREQ_H
#define PREREQ_H

#include <windows.h>

#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Check if a Windows optional feature is enabled (via DISM). */
ASB_API BOOL prereq_is_feature_enabled(const wchar_t *feature_name);

/* Progress callback for feature enable.  pct: 0.0 - 100.0 */
typedef void (*PrereqProgressCallback)(float pct, void *user_data);

/* Enable a Windows optional feature (via DISM). Sets *reboot_required on 3010.
   progress_cb is optional (may be NULL). */
ASB_API BOOL prereq_enable_feature(const wchar_t *feature_name, BOOL *reboot_required,
                                    PrereqProgressCallback progress_cb, void *user_data);

/* Check all HCS prerequisites.
   Returns TRUE if Virtual Machine Platform is enabled and HCS is available. */
ASB_API BOOL prereq_check_all(void);

/* Initiate a system reboot.  Returns TRUE if the reboot was initiated. */
ASB_API BOOL prereq_reboot(void);

#endif /* PREREQ_H */
