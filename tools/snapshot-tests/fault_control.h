/* The test program's control over the wrappers in fault_hooks.c. */
#ifndef FAULT_CONTROL_H
#define FAULT_CONTROL_H

#include <windows.h>

typedef enum {
    FAULT_NONE = 0,
    FAULT_FAIL,        /* this operation fails */
    FAULT_FAIL_ALL,    /* this one and every later one fail: rollback fails too */
    FAULT_KILL_BEFORE, /* the process dies just before it */
    FAULT_KILL_AFTER   /* the process dies just after it */
} FaultAction;

#define EXIT_KILLED 77
#define EXIT_FENCE  99

/* Nothing outside this folder may be moved, deleted or written. A breach ends the process. */
void fault_set_fence(const wchar_t *folder);

/* From now on count operations; `site` is the 1-based one that `action` applies to.
   With a trace file, each operation is also recorded there as it happens. */
void fault_arm(int site, FaultAction action, const wchar_t *trace_path);
void fault_disarm(void);

/* Stands for an operation outside snapshot.c, such as saving vms.cfg. TRUE = it should fail. */
BOOL fault_point(const char *name);
void fault_point_done(void);

/* The last operation snapshot.c performed, armed or not: says what a failure came from. */
const char *fault_last_op(void);

/* The product's function, with no fault hooks in the way. */
HRESULT vhdx_create_differencing_for_tests(const wchar_t *child_path, const wchar_t *parent_path);

#endif /* FAULT_CONTROL_H */
