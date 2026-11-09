#ifndef ASB_MAC_HEADLESS_VM_H
#define ASB_MAC_HEADLESS_VM_H

#include <stdint.h>
#include <stddef.h>
#include "../core/input_router.h"

enum {
    ASB_OK=0,ASB_DENIED=5,ASB_MEMORY=8,ASB_DATA=13,ASB_NOT_READY=21,
    ASB_WRITE=29,ASB_NOT_SUPPORTED=50,ASB_INVALID=87,ASB_PIPE=109,
    ASB_BUSY=170,ASB_NOT_FOUND=1168,ASB_CANCELLED=1223,ASB_TIMEOUT=1460
};

typedef struct {
    uint64_t id,run_id,ssh_session;
    void *channel;
    unsigned ssh_port;
    char name[256],os_type[32],user[128],key[2048],known_hosts[2048];
} AsbMacHeadlessTarget;

typedef struct AsbMacInput AsbMacInput;
typedef struct {
    uint32_t flags,generation,work_id,x,y,keyboard_version,mouse_version,width,height;
    int connected;
    char identity[321],vendor[321],model[321],purpose[1201];
} AsbMacInputStatus;

typedef struct {
    unsigned char *stdout_bytes,*stderr_bytes;
    uint32_t stdout_size,stderr_size,exit_code,stdin_error;
    int running;
} AsbMacSshOutput;

uint64_t asb_mac_milliseconds(void);
int asb_mac_target(uint64_t,AsbMacHeadlessTarget *);
void *asb_mac_channel_create(void *,void *,void *);
void *asb_mac_channel_display(void *);
int asb_mac_input_versions(int,int (*)(void *),void *,int *,int *);
int asb_mac_channel_connect(const AsbMacHeadlessTarget *,unsigned,unsigned);
void asb_mac_channel_retain(void *);
void asb_mac_channel_release(void *);
uint32_t asb_mac_channel_read(int,void *,size_t);
uint32_t asb_mac_channel_write(int,const void *,size_t);
AsbMacInput *asb_mac_input_session(const AsbMacHeadlessTarget *);
void asb_mac_input_release(AsbMacInput *);
void asb_mac_input_status(AsbMacInput *,AsbMacInputStatus *);
void asb_mac_input_context(uint64_t,AsbMacInputStatus *);
uint32_t asb_mac_input_claim(AsbMacInput *,const AsbMacInputStatus *);
uint32_t asb_mac_input_control(AsbMacInput *,uint32_t,uint32_t);
uint32_t asb_mac_input_send(AsbMacInput *,int,uint32_t,const InputPacket *);
uint32_t asb_mac_input_barrier(AsbMacInput *,uint32_t,unsigned);
int asb_mac_input_viewer(AsbMacInput *);
uint32_t asb_mac_input_begin_batch(AsbMacInput *);
void asb_mac_input_end_batch(AsbMacInput *);
void asb_mac_input_dimensions(uint64_t,unsigned,unsigned);
void asb_mac_input_forget(uint64_t);
uint32_t asb_mac_capture(const AsbMacHeadlessTarget *,unsigned,AsbScreenshotReply *,unsigned char **);
uint32_t asb_mac_native_capture(const AsbMacHeadlessTarget *,unsigned,AsbScreenshotReply *,unsigned char **);
uint32_t asb_mac_ssh_open(const AsbMacHeadlessTarget *,const char *,uint64_t *);
uint32_t asb_mac_ssh_read(uint64_t,uint64_t,unsigned,AsbMacSshOutput *);
uint32_t asb_mac_ssh_write(uint64_t,uint64_t,const void *,size_t,int);
uint32_t asb_mac_ssh_close(uint64_t,uint64_t);
void asb_mac_ssh_output_free(AsbMacSshOutput *);
void asb_mac_ssh_shutdown(uint64_t);

#endif
