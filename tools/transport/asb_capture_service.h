#pragma once
#include <stddef.h>
#include "asb_automation.h"

typedef struct {
    void *context;
    int (*receive)(void *,void *,void *,size_t);
    int (*send)(void *,void *,const void *,size_t);
    uint32_t (*capture)(void *,const AsbScreenshotRequest *,AsbScreenshotReply *,unsigned char **);
} AsbCaptureService;

#ifdef __cplusplus
extern "C" {
#endif
int asb_capture_service_request(const AsbCaptureService *,void *);
uint32_t asb_capture_pack(const void *,uint32_t,AsbScreenshotReply *,unsigned char **);
#ifdef __cplusplus
}
#endif
