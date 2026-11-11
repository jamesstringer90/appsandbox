#pragma once
#include <stdint.h>
#include <stddef.h>
#include "protocol.h"
#include "../../tools/transport/asb_automation.h"

typedef int (*AsbInputEmit)(void *context,const InputPacket *packet);
typedef struct {
    uint32_t generation,work_id,cancelled_work,owner,working,paused,cancelled;
    uint32_t cursor_x,cursor_y,buttons,transition,transition_id,sequence;
    uint8_t scan[2][256],vk[2][256],legacy[256];
    AsbInputEmit emit;
    void *context;
} AsbInputRouter;
void asb_input_router_init(AsbInputRouter *router,AsbInputEmit emit,void *context);
uint32_t asb_input_router_request(AsbInputRouter *router,uint32_t work);
uint32_t asb_input_router_control(AsbInputRouter *router,uint32_t command,uint32_t work);
uint32_t asb_input_router_send(AsbInputRouter *router,int agent,uint32_t generation,const InputPacket *packet);
void asb_input_router_observe(AsbInputRouter *router,int agent,const InputPacket *packet);
void asb_input_router_reply(AsbInputRouter *router,const InputPacket *packet);
void asb_input_router_disconnect(AsbInputRouter *router);
uint32_t asb_input_router_flags(const AsbInputRouter *router);
