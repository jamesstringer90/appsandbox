#include "headless_vm.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint32_t asb_mac_capture(const AsbMacHeadlessTarget *target,unsigned timeout,AsbScreenshotReply *reply,unsigned char **pixels)
{
    memset(reply,0,sizeof(*reply));*pixels=NULL;
    if (!timeout || timeout>10000) return ASB_INVALID;
    if (asb_mac_channel_display(target->channel)) return asb_mac_native_capture(target,timeout,reply,pixels);
    int fd=asb_mac_channel_connect(target,ASB_CH_SCREENSHOT,2000);
    if (fd<0) return ASB_NOT_READY;
    AsbScreenshotRequest request={ASB_SCREENSHOT_REQUEST_MAGIC,ASB_SCREENSHOT_VERSION,asb_mac_milliseconds(),timeout,0};
    uint32_t error=asb_mac_channel_write(fd,&request,sizeof(request));
    if (!error) error=asb_mac_channel_read(fd,reply,sizeof(*reply));
    if (!error && (reply->magic!=ASB_SCREENSHOT_REPLY_MAGIC || reply->version!=ASB_SCREENSHOT_VERSION ||
        reply->request_id!=request.request_id)) error=ASB_DATA;
    if (!error) error=reply->status;
    if (!error && (!reply->width || !reply->height || reply->width>7680 || reply->height>4320 ||
        reply->stride!=reply->width*4 || reply->data_size!=(uint64_t)reply->stride*reply->height ||
        (reply->flags&~ASB_SCREENSHOT_GPU_SHARED))) error=ASB_DATA;
    if (!error) {
        *pixels=malloc(reply->data_size);
        error=*pixels?asb_mac_channel_read(fd,*pixels,reply->data_size):ASB_MEMORY;
    }
    close(fd);
    if (error) { free(*pixels);*pixels=NULL; }
    return error;
}
