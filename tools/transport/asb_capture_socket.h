#pragma once
#include "asb_capture_service.h"

typedef struct AsbCaptureSocket AsbCaptureSocket;
AsbCaptureSocket *asb_capture_socket_start(int listener,void *context,
    uint32_t (*capture)(void *,const AsbScreenshotRequest *,AsbScreenshotReply *,unsigned char **));
void asb_capture_socket_stop(AsbCaptureSocket *);
