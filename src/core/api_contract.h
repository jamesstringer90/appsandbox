#pragma once
#include "api_json.h"
#include <stdint.h>
#include "protocol.h"

typedef struct {
    uint32_t type;
    int x,y,width,height,button,delta,scan,down,extended;
    wchar_t *text;
} AsbApiInputAction;

#define ASB_API_CLICK 100u
#define ASB_API_TEXT 101u
uint32_t asb_api_parse_input(const ApiJson *,AsbApiInputAction actions[128],int *count);
typedef struct {
    uint32_t (*send)(void *,const InputPacket *);
    uint32_t (*text)(void *,uint32_t);
    uint32_t (*barrier)(void *);
    void (*sleep)(unsigned);
} AsbApiInputOps;
uint32_t asb_api_perform_input(const AsbApiInputAction *,int,void *,const AsbApiInputOps *);

#define ASB_API_BODY_MAX 65535
#define ASB_API_DEFAULT_PORT 51152
#define ASB_API_MIN_RAM_MB 4096
#define ASB_API_DEFAULT_RAM_MAX_MB 12288
#define ASB_API_MIN_HDD_GB 64
#define ASB_API_MIN_LINUX_HDD_GB 40
#define ASB_API_DEFAULT_HDD_GB 64
#define ASB_API_DEFAULT_CPU_CORES 4
#define ASB_API_DEFAULT_NETWORK_MODE 1

static inline unsigned asb_api_default_ram_mb(uint64_t host_ram_mb)
{
    uint64_t ram=host_ram_mb/2;
    if (ram>ASB_API_DEFAULT_RAM_MAX_MB) ram=ASB_API_DEFAULT_RAM_MAX_MB;
    if (ram<ASB_API_MIN_RAM_MB) ram=ASB_API_MIN_RAM_MB;
    return (unsigned)(ram-ram%2);
}

static inline const char *asb_api_validate_ram_mb(int ram_mb)
{
    if (ram_mb<ASB_API_MIN_RAM_MB) return "RAM must be at least 4096 MB.";
    if (ram_mb%2) return "RAM must be 2 MB-aligned (an even number of MB).";
    return NULL;
}

static inline const char *asb_api_validate_hdd_gb(int hdd_gb,int linux_guest)
{
    if (linux_guest) return hdd_gb<ASB_API_MIN_LINUX_HDD_GB?"Disk size must be at least 40 GB.":NULL;
    return hdd_gb<ASB_API_MIN_HDD_GB?"Disk size must be at least 64 GB.":NULL;
}

typedef struct {
    const wchar_t *id,*backend,*name;
    const char *backend_utf8;
} AsbApiGuestOs;

static inline const AsbApiGuestOs *asb_api_guest_os(const wchar_t *os_type)
{
    static const AsbApiGuestOs types[]={
        {L"windows11",L"Windows",L"Windows 11","Windows"},
        {L"ubuntu2604lts",L"Linux",L"Ubuntu 26.04 LTS","Linux"},
        {L"macos27",L"macOS",L"macOS 27","macOS"}
    };
    if (!os_type) return NULL;
    for (unsigned i=0;i<sizeof(types)/sizeof(*types);++i) {
        const wchar_t *aliases[]={types[i].id,types[i].backend};
        for (unsigned j=0;j<2;++j) {
            const wchar_t *a=os_type,*b=aliases[j];
            for (;;) {
                wchar_t left=*a++,right=*b++;
                if (left>=L'A' && left<=L'Z') left+=L'a'-L'A';
                if (right>=L'A' && right<=L'Z') right+=L'a'-L'A';
                if (left!=right) break;
                if (!left) return &types[i];
            }
        }
    }
    return NULL;
}

static inline const AsbApiGuestOs *asb_api_guest_os_utf8(const char *os_type)
{
    wchar_t wide[32];unsigned i=0;
    if (!os_type) return NULL;
    for (;i<31 && os_type[i];++i) {
        if ((unsigned char)os_type[i]>127) return NULL;
        wide[i]=(unsigned char)os_type[i];
    }
    if (os_type[i]) return NULL;
    wide[i]=0;return asb_api_guest_os(wide);
}

static inline const wchar_t *asb_api_guest_os_type(const wchar_t *os_type)
{
    const AsbApiGuestOs *guest=asb_api_guest_os(os_type);
    return guest?guest->id:os_type;
}

static inline const wchar_t *asb_api_guest_os_name(const wchar_t *os_type)
{
    const AsbApiGuestOs *guest=asb_api_guest_os(os_type);
    return guest?guest->name:os_type;
}

int asb_api_validate_body(const wchar_t *body);
void asb_api_log_endpoints(unsigned port);
static inline int asb_api_parse_port(const wchar_t *value)
{
    unsigned port=0;
    if (!value || !*value) return 0;
    for (;*value;++value) {
        if (*value<L'0' || *value>L'9') return 0;
        port=port*10+(unsigned)(*value-L'0');
        if (port>65535) return 0;
    }
    return (int)port;
}

int asb_api_validate_request(const char *method,const char *path,const wchar_t *body);
char *asb_api_error_json(const char *code,const char *message);
typedef struct { int status;const char *code,*message; } AsbApiOperationError;
AsbApiOperationError asb_api_operation_error(uint32_t error,int input_operation);
int asb_api_validate_start(const wchar_t *body);
int asb_api_validate_start_options(int snapshot,int branch,int named_branch);
const wchar_t *asb_api_headless_route(const char *method,const wchar_t *path,uint64_t *session_id);
int asb_api_snapshot_route(const char *method,const wchar_t *path,int *snapshot,int *branch);
const char *asb_api_capabilities_json(int windows_host);
uint32_t asb_api_guest_support(int windows_host,const wchar_t *os_type,int is_template);
const char *asb_api_vm_fields_json(void);
