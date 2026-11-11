#pragma once
#include <stdint.h>

typedef struct AsbMcp AsbMcp;
typedef struct {
    void *context;
    uint64_t (*event_cursor)(void *);
    int (*events)(void *,uint64_t *,unsigned,unsigned,char **);
} AsbMcpEvents;
typedef struct { int status;char *json;char session[33]; } AsbMcpResponse;

AsbMcp *asb_mcp_create(unsigned port,const char *token,const char *version,const AsbMcpEvents *events);
void asb_mcp_destroy(AsbMcp *server);
void asb_mcp_request(AsbMcp *server,const char *method,const char *session,const char *protocol,const char *body,AsbMcpResponse *response);
int asb_mcp_origin_allowed(AsbMcp *server,const char *origin);
int asb_mcp_api_request(unsigned port,const char *token,const char *method,const char *path,const char *body,unsigned timeout,char **response);
