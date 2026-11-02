#include "asb_events.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK event_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&event_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&event_lock)
#else
#include <pthread.h>
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&event_lock)
#define UNLOCK() pthread_mutex_unlock(&event_lock)
#endif

typedef struct {
    unsigned id;
    CoreVmEventCallback callback;
    void *context;
} Subscription;

static Subscription subscribers[16];
static unsigned next_id;

unsigned asb_events_subscribe(CoreVmEventCallback callback, void *context)
{
    unsigned id = 0;
    if (!callback) return 0;
    LOCK();
    for (unsigned i = 0; i < 16; ++i) {
        if (subscribers[i].id) continue;
        if (++next_id == 0) ++next_id;
        id = next_id;
        subscribers[i].id = id;
        subscribers[i].callback = callback;
        subscribers[i].context = context;
        break;
    }
    UNLOCK();
    return id;
}

void asb_events_unsubscribe(unsigned id)
{
    LOCK();
    for (unsigned i = 0; i < 16; ++i)
        if (subscribers[i].id == id) memset(&subscribers[i], 0, sizeof(Subscription));
    UNLOCK();
}

void asb_events_emit(const CoreVmEvent *event)
{
    Subscription pending[16];
    LOCK();
    memcpy(pending, subscribers, sizeof(pending));
    UNLOCK();
    for (unsigned i = 0; i < 16; ++i)
        if (pending[i].id) pending[i].callback(event, pending[i].context);
}

void asb_events_changed(void)
{
    CoreVmEvent event = {0};
    event.type = CORE_VM_EVENT_LIST_CHANGED;
    asb_events_emit(&event);
}
