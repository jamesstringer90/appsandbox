#include "headless_vm.h"
#include "hcs_vm.h"

static INIT_ONCE startup=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK start_winsock(PINIT_ONCE once,void *parameter,void **context)
{
    WSADATA data;
    (void)once;(void)parameter;(void)context;
    return WSAStartup(MAKEWORD(2,2),&data)==0;
}

SOCKET asb_channel_connect(const AsbHeadlessTarget *target,unsigned channel,DWORD timeout)
{
    if (!InitOnceExecuteOnce(&startup,start_winsock,NULL,NULL)) return INVALID_SOCKET;
    struct { ADDRESS_FAMILY family;USHORT reserved;GUID vm,service; } address={0};
    address.family=34;
    address.vm=target->runtime;
    hcs_service_guid(target->os_type,channel,&address.service);
    SOCKET s=socket(34,SOCK_STREAM,1);
    if (s==INVALID_SOCKET) return s;
    u_long mode=1;
    ioctlsocket(s,FIONBIO,&mode);
    if (connect(s,(struct sockaddr *)&address,sizeof(address))) {
        if (WSAGetLastError()!=WSAEWOULDBLOCK) goto fail;
        fd_set write_set,error_set;
        FD_ZERO(&write_set);FD_ZERO(&error_set);
        FD_SET(s,&write_set);FD_SET(s,&error_set);
        struct timeval wait={(long)(timeout/1000),(long)(timeout%1000)*1000};
        if (select(0,NULL,&write_set,&error_set,&wait)<=0 || FD_ISSET(s,&error_set)) goto fail;
    }
    mode=0;
    ioctlsocket(s,FIONBIO,&mode);
    timeout=12000;
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char *)&timeout,sizeof(timeout));
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(char *)&timeout,sizeof(timeout));
    return s;
fail:
    closesocket(s);
    return INVALID_SOCKET;
}

DWORD asb_channel_read(SOCKET s,void *data,size_t size)
{
    BYTE *p=data;
    while (size) {
        int n=recv(s,(char *)p,(int)min(size,1024*1024),0);
        if (n<=0) return n?WSAGetLastError():ERROR_BROKEN_PIPE;
        p+=n;size-=n;
    }
    return 0;
}

DWORD asb_channel_write(SOCKET s,const void *data,size_t size)
{
    const BYTE *p=data;
    while (size) {
        int n=send(s,(const char *)p,(int)min(size,1024*1024),0);
        if (n<=0) return n?WSAGetLastError():ERROR_BROKEN_PIPE;
        p+=n;size-=n;
    }
    return 0;
}
