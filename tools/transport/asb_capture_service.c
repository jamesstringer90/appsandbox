#include "asb_capture_service.h"
#include <stdlib.h>
#include <string.h>

uint32_t asb_capture_pack(const void *source,uint32_t source_stride,
                          AsbScreenshotReply *reply,unsigned char **pixels)
{
    *pixels=NULL;
    if (!source || !reply->width || !reply->height || reply->width>7680 ||
        reply->height>4320 || source_stride<reply->width*4) return 13;
    reply->stride=reply->width*4;
    reply->data_size=reply->stride*reply->height;
    unsigned char *copy=malloc(reply->data_size);
    if (!copy) return 8;
    for (uint32_t row=0;row<reply->height;++row)
        memcpy(copy+(size_t)row*reply->stride,
            (const unsigned char *)source+(size_t)row*source_stride,reply->stride);
    *pixels=copy;
    return 0;
}

static int transfer(const AsbCaptureService *service,void *connection,void *data,size_t size,int sending)
{
    unsigned char *cursor=data;
    while (size) {
        int count=sending?service->send(service->context,connection,cursor,size)
                         :service->receive(service->context,connection,cursor,size);
        if (count<=0 || (size_t)count>size) return -1;
        cursor+=count;size-=(size_t)count;
    }
    return 0;
}

int asb_capture_service_request(const AsbCaptureService *service,void *connection)
{
    AsbScreenshotRequest request={0};
    AsbScreenshotReply reply={0};
    unsigned char *pixels=NULL;
    if (transfer(service,connection,&request,sizeof(request),0)<0) return -1;
    if (request.magic!=ASB_SCREENSHOT_REQUEST_MAGIC || request.version!=ASB_SCREENSHOT_VERSION ||
        request.flags || !request.timeout_ms || request.timeout_ms>10000)
        reply.status=87;
    else reply.status=service->capture(service->context,&request,&reply,&pixels);
    reply.magic=ASB_SCREENSHOT_REPLY_MAGIC;
    reply.version=ASB_SCREENSHOT_VERSION;
    reply.request_id=request.request_id;
    if (!reply.status && (!pixels || !reply.width || !reply.height ||
        reply.width>7680 || reply.height>4320 || reply.stride!=reply.width*4 ||
        reply.data_size!=(uint64_t)reply.stride*reply.height ||
        (reply.flags&~ASB_SCREENSHOT_GPU_SHARED))) reply.status=13;
    if (reply.status) reply.data_size=0;
    int sent=transfer(service,connection,&reply,sizeof(reply),1)==0;
    if (sent && reply.data_size) sent=transfer(service,connection,pixels,reply.data_size,1)==0;
    free(pixels);
    return sent?0:-1;
}
