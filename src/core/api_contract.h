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
#define ASB_API_DEFAULT_RAM_MB 4096
#define ASB_API_DEFAULT_HDD_GB 64
#define ASB_API_DEFAULT_CPU_CORES 4
#define ASB_API_DEFAULT_NETWORK_MODE 1

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
