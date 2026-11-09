#include "input_router.h"
#include <string.h>

static void generation(AsbInputRouter *r)
{
    if (!++r->generation) ++r->generation;
}

static int emit(AsbInputRouter *r,uint32_t type,uint32_t a,uint32_t b,uint32_t c)
{
    InputPacket packet={INPUT_MAGIC,type,a,b,c};
    return r->emit && r->emit(r->context,&packet);
}

static int release(AsbInputRouter *r)
{
    int ok=1;
    for (uint32_t e=0;e<2;++e) for (uint32_t k=0;k<256;++k)
        if (r->scan[e][k]) ok=emit(r,INPUT_KEY_PHYSICAL,0,k,e|INPUT_KEY_UP) && ok;
    for (uint32_t e=0;e<2;++e) for (uint32_t k=0;k<256;++k)
        if (r->vk[e][k]) ok=emit(r,INPUT_KEY_PHYSICAL,k,0,e|INPUT_KEY_UP) && ok;
    for (uint32_t k=0;k<256;++k)
        if (r->legacy[k]) ok=emit(r,INPUT_KEY,k,0,INPUT_KEY_UP) && ok;
    for (uint32_t b=0;b<3;++b)
        if (r->buttons&(1u<<b)) ok=emit(r,INPUT_MOUSE_BUTTON,b,0,0) && ok;
    memset(r->scan,0,sizeof(r->scan));
    memset(r->vk,0,sizeof(r->vk));
    memset(r->legacy,0,sizeof(r->legacy));
    r->buttons=0;
    return ok;
}

static uint32_t transfer(AsbInputRouter *r,uint32_t owner)
{
    generation(r);
    r->transition=owner+1;
    if (!++r->sequence) ++r->sequence;
    r->transition_id=r->sequence|0x80000000u;
    if (!release(r) || !emit(r,INPUT_MOUSE_QUERY,INPUT_MOUSE_NORMALIZED_VERSION,r->transition_id,0)) {
        asb_input_router_disconnect(r);
        return 29;
    }
    return 0;
}

void asb_input_router_init(AsbInputRouter *r,AsbInputEmit callback,void *context)
{
    memset(r,0,sizeof(*r));
    r->generation=1;
    r->cursor_x=r->cursor_y=UINT32_MAX;
    r->emit=callback;
    r->context=context;
}

uint32_t asb_input_router_request(AsbInputRouter *r,uint32_t work)
{
    if (!work) return 87;
    if (work==r->cancelled_work) return 1223;
    if (r->working && r->work_id!=work) return 170;
    if (!r->working) {
        r->working=1;
        r->work_id=work;
        r->cancelled=r->paused=0;
        r->cursor_x=r->cursor_y=UINT32_MAX;
    }
    if (r->paused || r->owner==ASB_CONTROL_AGENT || r->transition) return 0;
    return transfer(r,ASB_CONTROL_AGENT);
}

uint32_t asb_input_router_control(AsbInputRouter *r,uint32_t command,uint32_t work)
{
    if (work && work!=r->work_id) return 87;
    if (command==ASB_INPUT_USER_GRANT) {
        if (!r->working || r->cancelled) return r->cancelled?1223:87;
        r->paused=0;
        return transfer(r,ASB_CONTROL_AGENT);
    }
    if (command==ASB_INPUT_USER_REVOKE || command==ASB_INPUT_USER_CANCEL || command==ASB_INPUT_AGENT_END) {
        r->paused=command!=ASB_INPUT_AGENT_END;
        if (command==ASB_INPUT_USER_CANCEL) {
            r->cancelled=1;
            r->cancelled_work=r->work_id;
        }
        if (command!=ASB_INPUT_USER_REVOKE) r->working=0;
        if (command==ASB_INPUT_AGENT_END) r->cancelled=0;
        if (r->owner!=ASB_CONTROL_AGENT && !r->transition) { generation(r);return 0; }
        return transfer(r,ASB_CONTROL_USER);
    }
    return 87;
}

uint32_t asb_input_router_send(AsbInputRouter *r,int agent,uint32_t lease,const InputPacket *p)
{
    if (!p || p->magic!=INPUT_MAGIC) return 87;
    if (p->type==INPUT_MOUSE_POSITION_QUERY) return emit(r,p->type,p->param1,p->param2,p->param3)?0:29;
    if (agent && r->cancelled) return 1223;
    if (r->transition || (agent?(r->owner!=ASB_CONTROL_AGENT || lease!=r->generation || !r->working):r->owner==ASB_CONTROL_AGENT)) return 5;
    if (!emit(r,p->type,p->param1,p->param2,p->param3)) return 29;
    asb_input_router_observe(r,agent,p);
    return 0;
}

void asb_input_router_observe(AsbInputRouter *r,int agent,const InputPacket *p)
{
    if (p->type==INPUT_MOUSE_BUTTON && p->param1<3) {
        if (p->param2) r->buttons|=1u<<p->param1;
        else r->buttons&=~(1u<<p->param1);
    } else if (p->type==INPUT_KEY_PHYSICAL) {
        uint32_t key=p->param2?p->param2:p->param1;
        if (key<256) {
            uint8_t (*keys)[256]=p->param2?r->scan:r->vk;
            keys[!!(p->param3&INPUT_KEY_EXTENDED)][key]=!(p->param3&INPUT_KEY_UP);
        }
    } else if (p->type==INPUT_KEY && p->param1<256) {
        r->legacy[p->param1]=!(p->param3&INPUT_KEY_UP);
    }
    if (agent && p->type==INPUT_MOUSE_MOVE && p->param3==INPUT_MOUSE_NORMALIZED) {
        r->cursor_x=p->param1;
        r->cursor_y=p->param2;
    }
}

void asb_input_router_reply(AsbInputRouter *r,const InputPacket *p)
{
    if (p->type==INPUT_MOUSE_REPLY && r->transition && p->param2==r->transition_id) {
        r->owner=r->transition-1;
        r->transition=0;
    }
}

void asb_input_router_disconnect(AsbInputRouter *r)
{
    generation(r);
    memset(r->scan,0,sizeof(r->scan));
    memset(r->vk,0,sizeof(r->vk));
    memset(r->legacy,0,sizeof(r->legacy));
    r->buttons=r->transition=0;
    r->owner=ASB_CONTROL_USER;
    if (r->working) r->paused=1;
}

uint32_t asb_input_router_flags(const AsbInputRouter *r)
{
    uint32_t have=r->owner==ASB_CONTROL_AGENT && !r->transition;
    uint32_t flags=have?ASB_STATUS_AGENT_HAS_CONTROL:r->transition?0:ASB_STATUS_USER_HAS_CONTROL;
    if (r->working && !have) flags|=ASB_STATUS_AGENT_WAITING;
    if (!r->working) flags|=ASB_STATUS_AGENT_FINISHED;
    if (r->paused) flags|=ASB_STATUS_USER_PAUSED;
    if (r->cancelled) flags|=ASB_STATUS_AGENT_CANCELLED;
    return flags;
}
