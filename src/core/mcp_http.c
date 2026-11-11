#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET McpSocket;
#define MCP_INVALID INVALID_SOCKET
#define mcp_close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
typedef int McpSocket;
#define MCP_INVALID (-1)
#define mcp_close close
#endif
#include "mcp_server.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MCP_HTTP_LIMIT (256u*1024u*1024u)

static int write_all(McpSocket fd,const char *bytes,size_t size)
{
    while (size) {
        int amount=size>INT_MAX?INT_MAX:(int)size;
        int count=(int)send(fd,bytes,amount,0);
        if (count<=0) return 0;
        bytes+=count;size-=(size_t)count;
    }
    return 1;
}

static int header_equal(const char *start,size_t length,const char *name)
{
    if (length!=strlen(name)) return 0;
    for (size_t i=0;i<length;++i) {
        unsigned char c=(unsigned char)start[i];
        if (c>='A' && c<='Z') c+='a'-'A';
        if (c!=(unsigned char)name[i]) return 0;
    }
    return 1;
}

int asb_mcp_api_request(unsigned port,const char *token,const char *method,const char *path,const char *body,unsigned timeout,char **response)
{
    McpSocket fd=MCP_INVALID;
    char *bytes=NULL,*result=NULL;
    size_t size=0,capacity=16384,header_size=0,length=0;
    int status=503,chunked=0,has_length=0,complete=0;
    *response=NULL;
#ifdef _WIN32
    WSADATA wsa;
    int wsa_started=0;
    if (WSAStartup(MAKEWORD(2,2),&wsa)) goto done;
    wsa_started=1;
    fd=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_NO_HANDLE_INHERIT);
#else
    fd=socket(AF_INET,SOCK_STREAM,0);
    if (fd!=MCP_INVALID) fcntl(fd,F_SETFD,FD_CLOEXEC);
#endif
    if (fd==MCP_INVALID) goto done;
#ifdef _WIN32
    DWORD wait=timeout;
#else
    struct timeval wait={.tv_sec=timeout/1000,.tv_usec=(timeout%1000)*1000};
#endif
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char *)&wait,sizeof(wait));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(const char *)&wait,sizeof(wait));
    struct sockaddr_in address={0};
    address.sin_family=AF_INET;address.sin_port=htons((unsigned short)port);
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (connect(fd,(struct sockaddr *)&address,sizeof(address))) goto done;
    char header[4096];
    size_t body_size=body?strlen(body):0;
    int count=snprintf(header,sizeof(header),
        "%s /v1%s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        method,path,port,token,body_size);
    if (count<=0 || (size_t)count>=sizeof(header) || !write_all(fd,header,(size_t)count) ||
        (body_size && !write_all(fd,body,body_size))) goto done;
    bytes=malloc(capacity+1);
    if (!bytes) goto done;
    for (;;) {
        if (size==capacity) {
            if (capacity>=MCP_HTTP_LIMIT) goto done;
            size_t grown=capacity*2;
            char *allocation=realloc(bytes,grown+1);
            if (!allocation) goto done;
            bytes=allocation;capacity=grown;
        }
        count=(int)recv(fd,bytes+size,(int)(capacity-size),0);
        if (count<0) goto done;
        if (!count) { complete=header_size!=0;break; }
        size+=(size_t)count;bytes[size]=0;
        if (!header_size) {
            char *end=strstr(bytes,"\r\n\r\n");
            if (!end) { if (size>16384) goto done;continue; }
            header_size=(size_t)(end-bytes)+4;
            char *code=strchr(bytes,' '),*after_status=NULL;
            if (strncmp(bytes,"HTTP/",5) || !code) goto done;
            long parsed=strtol(code+1,&after_status,10);
            if (after_status!=code+4 || parsed<200 || parsed>599) goto done;
            status=(int)parsed;
            for (char *line=strstr(bytes,"\r\n")+2;line<end;) {
                char *next=strstr(line,"\r\n");
                if (!next) goto done;
                char *colon=memchr(line,':',(size_t)(next-line));
                if (!colon) goto done;
                char *value=colon+1;
                while (value<next && (*value==' ' || *value=='\t')) ++value;
                if (header_equal(line,(size_t)(colon-line),"content-length")) {
                    char *after=NULL;
                    unsigned long long parsed_length=strtoull(value,&after,10);
                    if (has_length || after!=next || parsed_length>MCP_HTTP_LIMIT) goto done;
                    length=(size_t)parsed_length;has_length=1;
                } else if (header_equal(line,(size_t)(colon-line),"transfer-encoding")) {
                    chunked=header_equal(value,(size_t)(next-value),"chunked");
                    if (!chunked) goto done;
                }
                line=next+2;
            }
        }
        if (has_length && !chunked && size-header_size>=length) { complete=1;break; }
        if (chunked && size>=header_size+5 && !memcmp(bytes+size-5,"0\r\n\r\n",5)) { complete=1;break; }
    }
    if (!complete) goto done;
    if (!chunked) {
        if (!has_length) length=size-header_size;
        if (length>size-header_size) goto done;
        result=malloc(length+1);
        if (!result) goto done;
        memcpy(result,bytes+header_size,length);result[length]=0;
    } else {
        size_t position=header_size,written=0;
        result=malloc(size-header_size+1);
        if (!result) goto done;
        for (;;) {
            char *after=NULL;
            unsigned long long amount=strtoull(bytes+position,&after,16);
            if (after==bytes+position || after+2>bytes+size || after[0]!='\r' || after[1]!='\n') { free(result);result=NULL;goto done; }
            position=(size_t)(after-bytes)+2;
            if (!amount) break;
            if (amount>size-position || amount>MCP_HTTP_LIMIT || position+(size_t)amount+2>size ||
                bytes[position+(size_t)amount]!='\r' || bytes[position+(size_t)amount+1]!='\n') { free(result);result=NULL;goto done; }
            memcpy(result+written,bytes+position,(size_t)amount);written+=(size_t)amount;
            position+=(size_t)amount+2;
        }
        result[written]=0;
    }
done:
    if (fd!=MCP_INVALID) mcp_close(fd);
#ifdef _WIN32
    if (wsa_started) WSACleanup();
#endif
    free(bytes);
    if (!result) {
        const char *error="{\"error\":{\"code\":\"api_unavailable\",\"message\":\"The local API did not return a complete response.\"}}";
        result=malloc(strlen(error)+1);
        if (result) memcpy(result,error,strlen(error)+1);
        status=503;
    }
    *response=result;
    return status;
}
