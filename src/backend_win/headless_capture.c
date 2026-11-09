#include "headless_vm.h"
#include <stdlib.h>
#include <string.h>

ASB_API DWORD asb_capture(const AsbHeadlessTarget *target,DWORD timeout,AsbCapture *capture)
{
    memset(capture,0,sizeof(*capture));
    if (timeout<1 || timeout>10000) return ERROR_INVALID_PARAMETER;
    SOCKET s=asb_channel_connect(target,ASB_CH_SCREENSHOT,2000);
    if (s==INVALID_SOCKET) return ERROR_NOT_READY;
    AsbScreenshotRequest request={ASB_SCREENSHOT_REQUEST_MAGIC,ASB_SCREENSHOT_VERSION,GetTickCount64(),timeout,0};
    DWORD error=asb_channel_write(s,&request,sizeof(request));
    AsbScreenshotReply *r=&capture->header;
    if (!error) error=asb_channel_read(s,r,sizeof(*r));
    if (!error && (r->magic!=ASB_SCREENSHOT_REPLY_MAGIC || r->version!=ASB_SCREENSHOT_VERSION || r->request_id!=request.request_id)) error=ERROR_INVALID_DATA;
    if (!error) error=r->status;
    if (!error && (!r->width || !r->height || r->width>7680 || r->height>4320 ||
        r->stride!=r->width*4 || r->data_size!=r->stride*r->height ||
        (r->flags&~ASB_SCREENSHOT_GPU_SHARED))) error=ERROR_INVALID_DATA;
    if (!error) {
        capture->pixels=malloc(r->data_size);
        if (!capture->pixels) error=ERROR_NOT_ENOUGH_MEMORY;
        else error=asb_channel_read(s,capture->pixels,r->data_size);
    }
    closesocket(s);
    if (error) asb_capture_free(capture);
    return error;
}

ASB_API void asb_capture_free(AsbCapture *capture)
{
    free(capture->pixels);
    memset(capture,0,sizeof(*capture));
}
