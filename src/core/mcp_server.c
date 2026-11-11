#include "mcp_server.h"
#include "api_json.h"
#include "api_contract.h"
#include "api_operations.h"
#include "asb_vm_id.h"
#include "asb_template_id.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

typedef struct { char *bytes;size_t length,capacity;int failed; } Buffer;
typedef struct { ApiJson json;wchar_t *text;ApiJsonToken *tokens; } Document;
typedef struct {
    char id[33];unsigned references,subscription,reading;int ready,deleted;
    uint64_t cursor;time_t touched;
} Session;
struct AsbMcp {
    unsigned port;char token[64],version[64];Document catalog;AsbMcpEvents events;
    Session sessions[64];
#ifdef _WIN32
    SRWLOCK lock;
#else
    pthread_mutex_t lock;
#endif
};

static void lock(AsbMcp *s)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}
static void unlock(AsbMcp *s)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}
static void append(Buffer *b,const char *text,size_t size)
{
    if (b->failed || size>SIZE_MAX-b->length-1) { b->failed=1;return; }
    size_t required=b->length+size+1;
    if (required>b->capacity) {
        size_t capacity=b->capacity?b->capacity:256;
        while (capacity<required) {
            if (capacity>SIZE_MAX/2) { b->failed=1;return; }
            capacity*=2;
        }
        char *allocation=realloc(b->bytes,capacity);
        if (!allocation) { b->failed=1;return; }
        b->bytes=allocation;b->capacity=capacity;
    }
    memcpy(b->bytes+b->length,text,size);b->length+=size;b->bytes[b->length]=0;
}
static void text(Buffer *b,const char *value) { append(b,value,strlen(value)); }
static char *finish(Buffer *b)
{
    if (b->failed) { free(b->bytes);return NULL; }
    if (!b->bytes) text(b,"");
    return b->bytes;
}
static void quote(Buffer *b,const char *value)
{
    text(b,"\"");
    for (const unsigned char *p=(const unsigned char *)value;*p;++p) {
        if (*p=='"' || *p=='\\') { text(b,"\\");append(b,(const char *)p,1); }
        else if (*p<32) { char escaped[7];snprintf(escaped,sizeof(escaped),"\\u%04x",*p);text(b,escaped); }
        else append(b,(const char *)p,1);
    }
    text(b,"\"");
}
static wchar_t *wide(const char *value)
{
    size_t length=strlen(value),position=0;
    wchar_t *result=calloc(length+1,sizeof(*result));
    if (!result) return NULL;
    for (size_t i=0;i<length;) {
        unsigned c=(unsigned char)value[i++],minimum=0,count=0;
        if (c<0x80) {}
        else if ((c&0xe0)==0xc0) { c&=31;count=1;minimum=0x80; }
        else if ((c&0xf0)==0xe0) { c&=15;count=2;minimum=0x800; }
        else if ((c&0xf8)==0xf0) { c&=7;count=3;minimum=0x10000; }
        else goto failed;
        if (i+count>length) goto failed;
        while (count--) {
            unsigned next=(unsigned char)value[i++];
            if ((next&0xc0)!=0x80) goto failed;
            c=(c<<6)|(next&63);
        }
        if (c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) goto failed;
#if WCHAR_MAX <= 0xffff
        if (c>=0x10000) { c-=0x10000;result[position++]=(wchar_t)(0xd800+(c>>10));c=0xdc00+(c&1023); }
#endif
        result[position++]=(wchar_t)c;
    }
    return result;
failed:
    free(result);return NULL;
}
static char *narrow(const wchar_t *value,size_t length)
{
    Buffer b={0};
    for (size_t i=0;i<length;++i) {
        unsigned c=(unsigned)value[i];char bytes[4];size_t count;
        if (c>=0xd800 && c<=0xdbff) {
            if (++i>=length || value[i]<0xdc00 || value[i]>0xdfff) { b.failed=1;break; }
            c=0x10000+((c-0xd800)<<10)+(unsigned)value[i]-0xdc00;
        } else if (c>=0xdc00 && c<=0xdfff) { b.failed=1;break; }
        if (c<0x80) { bytes[0]=(char)c;count=1; }
        else if (c<0x800) { bytes[0]=(char)(0xc0|(c>>6));bytes[1]=(char)(0x80|(c&63));count=2; }
        else if (c<0x10000) { bytes[0]=(char)(0xe0|(c>>12));bytes[1]=(char)(0x80|((c>>6)&63));bytes[2]=(char)(0x80|(c&63));count=3; }
        else if (c<=0x10ffff) { bytes[0]=(char)(0xf0|(c>>18));bytes[1]=(char)(0x80|((c>>12)&63));bytes[2]=(char)(0x80|((c>>6)&63));bytes[3]=(char)(0x80|(c&63));count=4; }
        else { b.failed=1;break; }
        append(&b,bytes,count);
    }
    return finish(&b);
}
static int parse(Document *d,const char *json)
{
    memset(d,0,sizeof(*d));d->text=wide(json);d->tokens=calloc(8192,sizeof(*d->tokens));
    return d->text && d->tokens && api_json_parse(&d->json,d->text,d->tokens,8192);
}
static void dispose(Document *d) { free(d->text);free(d->tokens);memset(d,0,sizeof(*d)); }
static char *slice(const Document *d,int token)
{
    if (token<0 || token>=d->json.count) return NULL;
    return narrow(d->text+d->tokens[token].start,(size_t)(d->tokens[token].end-d->tokens[token].start));
}
static char *string(const Document *d,int token)
{
    if (token<0 || token>=d->json.count) return NULL;
    size_t capacity=(size_t)(d->tokens[token].end-d->tokens[token].start)+1;
    wchar_t *decoded=calloc(capacity,sizeof(*decoded));
    char *result=NULL;
    if (decoded && api_json_string(&d->json,token,decoded,capacity)) result=narrow(decoded,wcslen(decoded));
    free(decoded);return result;
}
static int field(const Document *d,int object,const wchar_t *name) { return api_json_field(&d->json,object,name); }
static void error(AsbMcpResponse *r,const char *id,int code,const char *message)
{
    Buffer b={0};char number[32];snprintf(number,sizeof(number),"%d",code);
    text(&b,"{\"jsonrpc\":\"2.0\",\"id\":");text(&b,id?id:"null");
    text(&b,",\"error\":{\"code\":");text(&b,number);text(&b,",\"message\":");quote(&b,message);text(&b,"}}");
    r->json=finish(&b);
}
static char *ssh_text(const Document *d,const wchar_t *key)
{
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *encoded=string(d,field(d,0,key));Buffer decoded={0},plain={0};unsigned bits=0,value=0;
    if (!encoded) return NULL;
    for (const char *p=encoded;*p && *p!='=';++p) {
        const char *digit=strchr(alphabet,*p);
        if (!digit) { decoded.failed=1;break; }
        value=(value<<6)|(unsigned)(digit-alphabet);bits+=6;
        if (bits>=8) { bits-=8;char byte=(char)(value>>bits);append(&decoded,&byte,1); }
    }
    free(encoded);
    if (decoded.failed) { free(decoded.bytes);return NULL; }
    for (size_t i=0;i<decoded.length;) {
        unsigned c=(unsigned char)decoded.bytes[i],amount=1,minimum=0;
        if (!c) { text(&plain,"\\0");++i;continue; }
        if (c<0x80) {}
        else if ((c&0xe0)==0xc0) { c&=31;amount=2;minimum=0x80; }
        else if ((c&0xf0)==0xe0) { c&=15;amount=3;minimum=0x800; }
        else if ((c&0xf8)==0xf0) { c&=7;amount=4;minimum=0x10000; }
        else amount=0;
        int valid=amount && i+amount<=decoded.length;
        for (unsigned j=1;valid && j<amount;++j) {
            unsigned next=(unsigned char)decoded.bytes[i+j];
            if ((next&0xc0)!=0x80) valid=0;
            c=(c<<6)|(next&63);
        }
        if (!valid || c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) {
            text(&plain,"\xef\xbf\xbd");++i;
        } else { append(&plain,decoded.bytes+i,amount);i+=amount; }
    }
    free(decoded.bytes);return finish(&plain);
}
static char *tool_result(int status,const char *json)
{
    Document document={0};Buffer b={0};char *metadata=NULL,*image=NULL;
    if (!json || !parse(&document,json)) { dispose(&document);return NULL; }
    int image_token=field(&document,0,L"image"),metadata_token=field(&document,0,L"metadata");
    if (status<400 && image_token>=0 && metadata_token>=0) {
        metadata=slice(&document,metadata_token);image=slice(&document,image_token);
        if (!metadata || !image) goto failed;
        text(&b,"{\"content\":[{\"type\":\"text\",\"text\":");quote(&b,metadata);
        text(&b,"},{\"type\":\"image\",");
        if (image[0]!='{' || strlen(image)<2) goto failed;
        append(&b,image+1,strlen(image)-1);text(&b,"],\"structuredContent\":");text(&b,metadata);text(&b,"}");
    } else if (status<400 && field(&document,0,L"stdoutBase64")>=0) {
        Buffer summary={0};int count=0;text(&summary,"{");
        for (int i=1;i<document.tokens[0].next;) {
            int value=document.tokens[i].next;char *key=string(&document,i),*part=slice(&document,value);
            if (!key || !part) summary.failed=1;
            else if (strcmp(key,"stdoutBase64") && strcmp(key,"stderrBase64")) {
                if (count++) text(&summary,",");quote(&summary,key);text(&summary,":");text(&summary,part);
            }
            free(key);free(part);i=document.tokens[value].next;
        }
        text(&summary,"}");metadata=finish(&summary);
        if (!metadata) goto failed;
        text(&b,"{\"content\":[{\"type\":\"text\",\"text\":");quote(&b,metadata);text(&b,"}");
        const wchar_t *keys[]={L"stdoutBase64",L"stderrBase64"};
        const char *labels[]={"stdout:\n","stderr:\n"};
        for (int i=0;i<2;++i) {
            char *output=ssh_text(&document,keys[i]);
            if (output && output[0]) {
                Buffer message={0};text(&message,labels[i]);text(&message,output);char *value=finish(&message);
                if (!value) b.failed=1;
                else { text(&b,",{\"type\":\"text\",\"text\":");quote(&b,value);text(&b,"}"); }
                free(value);
            }
            free(output);
        }
        text(&b,"],\"structuredContent\":");text(&b,json);text(&b,"}");
    } else {
        text(&b,"{\"isError\":");text(&b,status>=400?"true":"false");
        text(&b,",\"content\":[{\"type\":\"text\",\"text\":");quote(&b,json);
        text(&b,"}],\"structuredContent\":");
        if (document.tokens[0].type==API_JSON_OBJECT) text(&b,json);
        else { text(&b,"{\"result\":");text(&b,json);text(&b,"}"); }
        text(&b,"}");
    }
    free(metadata);free(image);dispose(&document);return finish(&b);
failed:
    free(metadata);free(image);dispose(&document);free(b.bytes);return NULL;
}
static char *tool_error(const char *message)
{
    Buffer b={0};text(&b,"{\"error\":{\"code\":\"invalid_arg\",\"message\":");quote(&b,message);text(&b,"}}");
    char *json=finish(&b),*result=json?tool_result(400,json):NULL;free(json);return result;
}
static const char *instructions=
    "AppSandbox exposes one host MCP backed by the same API used for VM operations. "
    "Discover VMs with vm_list and templates with template_list. Use their explicit vm_id or template_id. "
    "Use windows11 for Windows 11, ubuntu2604lts for Ubuntu 26.04 LTS, and macos27 for macOS 27. No other Linux distribution or version is supported. Use osName and capabilities.guestOsNames for display labels. "
    "Set openDisplay to false for windowless starts. "
    "Before desktop input call input_request_control with your identity, model vendor, exact model identifier, "
    "and a purpose of at most 300 characters. Use unknown when vendor or model information is unavailable. "
    "Check agent_has_control before input. When the user takes control, stop input and await input_wait_for_control; "
    "do not retry input or force takeover. Return control resumes unfinished work. "
    "Call input_finish when finished, cancelled, or abandoned. If agent_cancelled is true, acknowledge with input_finish "
    "and stop; do not start again without a new user instruction. Starting new work needs no approval. "
    "Screenshots and SSH do not acquire input or activate the overlay. Close SSH sessions when finished. "
    "Viewer presence is informational. Subscribe to existing API events with events_subscribe, read with events_read, "
    "and call events_unsubscribe when finished. Events have no replay. Refresh status after dropped events or stream end. "
    "Use input_status for input ownership.";
static const char *event_tools=
    "{\"name\":\"events_subscribe\",\"description\":\"Subscribe to future events from the existing API status stream.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
    "{\"name\":\"events_read\",\"description\":\"Read bounded status events; refresh API state after dropped events or stream end.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"timeoutMs\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":30000},\"maxEvents\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":64}},\"additionalProperties\":false}},"
    "{\"name\":\"events_unsubscribe\",\"description\":\"Stop this MCP session's event subscription.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}";

static char *tools(AsbMcp *server)
{
    Buffer b={0};int array=field(&server->catalog,0,L"operations"),count=0;
    text(&b,"{\"tools\":[");
    for (int i=array+1;i<server->catalog.tokens[array].next;i=server->catalog.tokens[i].next) {
        if (count++) text(&b,",");
        text(&b,"{");
        const wchar_t *names[]={L"name",L"description",L"inputSchema"};
        for (int j=0;j<3;++j) {
            char *key=narrow(names[j],wcslen(names[j])),*value=slice(&server->catalog,field(&server->catalog,i,names[j]));
            if (!key || !value) b.failed=1;
            if (j) text(&b,",");
            if (key) quote(&b,key);text(&b,":");if (value) text(&b,value);
            free(key);free(value);
        }
        text(&b,"}");
    }
    if (count) text(&b,",");text(&b,event_tools);text(&b,"]}");return finish(&b);
}
static int protocol_supported(const char *version)
{
    return !strcmp(version,"2025-03-26") || !strcmp(version,"2025-06-18") || !strcmp(version,"2025-11-25");
}
AsbMcp *asb_mcp_create(unsigned port,const char *token,const char *version,const AsbMcpEvents *events)
{
    AsbMcp *s=calloc(1,sizeof(*s));
    if (!s || strlen(token)>=sizeof(s->token) || strlen(version)>=sizeof(s->version)) { free(s);return NULL; }
    s->port=port;memcpy(s->token,token,strlen(token)+1);memcpy(s->version,version,strlen(version)+1);
    if (events) s->events=*events;
#ifdef _WIN32
    InitializeSRWLock(&s->lock);
#else
    pthread_mutex_init(&s->lock,NULL);
#endif
    if (!parse(&s->catalog,asb_api_operations_json())) { asb_mcp_destroy(s);return NULL; }
    return s;
}
void asb_mcp_destroy(AsbMcp *s)
{
    if (!s) return;
    dispose(&s->catalog);
#ifndef _WIN32
    pthread_mutex_destroy(&s->lock);
#endif
    memset(s->token,0,sizeof(s->token));free(s);
}
int asb_mcp_origin_allowed(AsbMcp *s,const char *origin)
{
    char expected[80];
    if (!origin || !origin[0]) return 1;
    snprintf(expected,sizeof(expected),"http://127.0.0.1:%u",s->port);
    if (!strcmp(origin,expected)) return 1;
    snprintf(expected,sizeof(expected),"http://localhost:%u",s->port);
    return !strcmp(origin,expected);
}
static Session *session(AsbMcp *s,const char *id,int create)
{
    Session *result=NULL,*available=NULL;time_t now=time(NULL);
    lock(s);
    for (size_t i=0;i<sizeof(s->sessions)/sizeof(*s->sessions);++i) {
        Session *current=&s->sessions[i];
        if (!current->references && (current->deleted || !current->id[0] || now-current->touched>1800)) {
            if (!available) available=current;
            continue;
        }
        if (!create && id && !strcmp(current->id,id) && !current->deleted) { result=current;break; }
    }
    if (create && available) {
        uint64_t first=asb_vm_id_new(),second=asb_vm_id_new();
        if (first && second) {
            memset(available,0,sizeof(*available));
            snprintf(available->id,sizeof(available->id),"%016llx%016llx",(unsigned long long)first,(unsigned long long)second);
            result=available;
        }
    }
    if (result) { ++result->references;result->touched=now; }
    unlock(s);return result;
}
static char *events(AsbMcp *s,Session *client,const char *name,const Document *request,int arguments)
{
    int timeout=1000,limit=64;
    if (!s->events.event_cursor || !s->events.events) return tool_error("Status events are unavailable.");
    if (arguments>=0) for (int i=arguments+1;i<request->tokens[arguments].next;) {
        wchar_t key[128];
        if (!api_json_string(&request->json,i,key,sizeof(key)/sizeof(*key)) || strcmp(name,"events_read") ||
            (wcscmp(key,L"timeoutMs") && wcscmp(key,L"maxEvents"))) return tool_error("Unexpected event arguments.");
        i=request->tokens[request->tokens[i].next].next;
    }
    if (!strcmp(name,"events_subscribe")) {
        uint64_t cursor=s->events.event_cursor(s->events.context);
        lock(s);
        if (!client->subscription) { client->cursor=cursor;client->subscription=1; }
        unlock(s);
        return tool_result(200,"{\"subscribed\":true,\"connected\":true,\"streamEnded\":false,\"streamError\":null,\"pendingEvents\":0,\"droppedEvents\":0}");
    }
    if (!strcmp(name,"events_unsubscribe")) {
        lock(s);client->subscription=0;unlock(s);
        return tool_result(200,"{\"subscribed\":false,\"connected\":false}");
    }
    if (arguments>=0) {
        int token=field(request,arguments,L"timeoutMs");
        if (token!=-1 && !api_json_int(&request->json,token,&timeout)) return tool_error("timeoutMs must be an integer.");
        token=field(request,arguments,L"maxEvents");
        if (token!=-1 && !api_json_int(&request->json,token,&limit)) return tool_error("maxEvents must be an integer.");
    }
    if (timeout<0 || timeout>30000 || limit<1 || limit>64) return tool_error("Invalid event read limits.");
    lock(s);
    if (!client->subscription || client->reading) { unlock(s);return tool_error("Subscribe before reading; only one event read can be pending per session."); }
    client->reading=1;uint64_t cursor=client->cursor;unlock(s);
    char *json=NULL;int status=s->events.events(s->events.context,&cursor,(unsigned)timeout,(unsigned)limit,&json);
    lock(s);client->cursor=cursor;client->reading=0;unlock(s);
    char *result=json?tool_result(status,json):tool_error("Status events are unavailable.");free(json);return result;
}
static char *call_tool(AsbMcp *s,Session *client,const Document *request,int params)
{
    char *name=string(request,field(request,params,L"name")),*result=NULL;
    int arguments=field(request,params,L"arguments"),operation=-1;
    if (!name || (arguments!=-1 && (arguments<0 || request->tokens[arguments].type!=API_JSON_OBJECT))) { free(name);return tool_error("A tool name and object arguments are required."); }
    if (!strcmp(name,"events_subscribe") || !strcmp(name,"events_read") || !strcmp(name,"events_unsubscribe")) {
        result=events(s,client,name,request,arguments);free(name);return result;
    }
    int array=field(&s->catalog,0,L"operations");
    for (int i=array+1;i<s->catalog.tokens[array].next;i=s->catalog.tokens[i].next) {
        char *candidate=string(&s->catalog,field(&s->catalog,i,L"name"));
        int match=candidate && !strcmp(name,candidate);free(candidate);
        if (match) { operation=i;break; }
    }
    free(name);
    if (operation<0) return tool_error("Unknown API operation.");
    char *method=string(&s->catalog,field(&s->catalog,operation,L"method"));
    char *pattern=string(&s->catalog,field(&s->catalog,operation,L"path"));
    Buffer path={0},body={0};int count=0,valid=method && pattern;
    if (pattern) for (const char *p=pattern;*p;) {
        if (*p!='{') { append(&path,p++,1);continue; }
        const char *end=strchr(p,'}');
        if (!end || end-p>=64) { valid=0;break; }
        wchar_t key[64];size_t n=(size_t)(end-p-1);
        for (size_t i=0;i<n;++i) key[i]=(unsigned char)p[i+1];key[n]=0;
        int token=field(request,arguments,key);
        if (token<0) { valid=0;break; }
        char *value=NULL;
        if (n>=6 && !wcscmp(key+n-6,L"_index")) {
            int index;
            if (!api_json_int(&request->json,token,&index)) { valid=0;break; }
            char number[32];snprintf(number,sizeof(number),"%d",index);text(&path,number);
        } else {
            value=string(request,token);
            if (!value) { valid=0;break; }
            if (!wcscmp(key,L"vm_id")) valid=asb_vm_id_parse(value)!=0;
            else if (!wcscmp(key,L"template_id")) valid=asb_template_id_parse(value)!=0;
            else if (!wcscmp(key,L"ssh_session_id")) valid=strlen(value)==20 && !strncmp(value,"ssh-",4) && strspn(value+4,"0123456789abcdef")==16;
            else valid=0;
            if (valid) text(&path,value);free(value);
        }
        p=end+1;
    }
    int schema=field(&s->catalog,operation,L"inputSchema"),properties=field(&s->catalog,schema,L"properties");
    text(&body,"{");
    if (arguments>=0) for (int i=arguments+1;i<request->tokens[arguments].next;) {
        int value=request->tokens[i].next;wchar_t key[128];
        if (!api_json_string(&request->json,i,key,sizeof(key)/sizeof(*key)) || field(&s->catalog,properties,key)<0) { valid=0;break; }
        char *key_text=string(request,i),*value_text=slice(request,value);Buffer marker={0};
        text(&marker,"{");if (key_text) text(&marker,key_text);text(&marker,"}");char *placeholder=finish(&marker);
        if (!key_text || !value_text || !placeholder) valid=0;
        else if (!pattern || !strstr(pattern,placeholder)) {
            if (method && !strcmp(method,"GET")) valid=0;
            if (count++) text(&body,",");quote(&body,key_text);text(&body,":");text(&body,value_text);
        }
        free(key_text);free(value_text);free(placeholder);i=request->tokens[value].next;
    }
    text(&body,"}");char *route=finish(&path),*payload=finish(&body);
    if (!route || !payload) valid=0;
    if (valid) {
        int timeout=60000,token=field(request,arguments,L"timeoutMs");
        if (token>=0 && api_json_int(&request->json,token,&timeout)) timeout+=5000;
        if (timeout<5000 || timeout>65000) timeout=65000;
        char *json=NULL;int status=asb_mcp_api_request(s->port,s->token,method,route,payload,(unsigned)timeout,&json);
        if (json) result=tool_result(status,json);free(json);
    } else result=tool_error("Invalid, missing, or unexpected tool arguments. Use explicit VM and template IDs.");
    free(method);free(pattern);free(route);free(payload);return result;
}
void asb_mcp_request(AsbMcp *s,const char *verb,const char *session_id,const char *protocol,const char *body,AsbMcpResponse *response)
{
    Document request={0};Session *client=NULL;char *id=NULL,*method=NULL,*result=NULL;
    memset(response,0,sizeof(*response));response->status=200;
    if (protocol && protocol[0] && !protocol_supported(protocol)) { response->status=400;error(response,NULL,-32600,"Unsupported MCP protocol version.");return; }
    if (!strcmp(verb,"GET")) { response->status=405;return; }
    if (!strcmp(verb,"DELETE")) {
        client=session(s,session_id,0);
        if (!client) { response->status=session_id && session_id[0]?404:400;return; }
        lock(s);client->deleted=1;client->subscription=0;--client->references;unlock(s);return;
    }
    if (strcmp(verb,"POST")) { response->status=405;return; }
    if (!body || !parse(&request,body) || request.tokens[0].type!=API_JSON_OBJECT || !asb_api_validate_body(request.text)) {
        response->status=400;error(response,NULL,-32700,"Invalid JSON-RPC request.");goto done;
    }
    int id_token=field(&request,0,L"id");
    if (id_token>=0) {
        if (request.tokens[id_token].type!=API_JSON_NUMBER && request.tokens[id_token].type!=API_JSON_STRING) { error(response,NULL,-32600,"Invalid request ID.");goto done; }
        id=slice(&request,id_token);
    }
    char *rpc=string(&request,field(&request,0,L"jsonrpc"));
    method=string(&request,field(&request,0,L"method"));
    int valid=rpc && !strcmp(rpc,"2.0") && method;free(rpc);
    if (!valid) { error(response,id,-32600,"Expected JSON-RPC 2.0 and a method.");goto done; }
    int params=field(&request,0,L"params");
    if (params!=-1 && (params<0 || request.tokens[params].type!=API_JSON_OBJECT)) { error(response,id,-32602,"params must be an object.");goto done; }
    if (!strcmp(method,"initialize")) {
        if (!id || (session_id && session_id[0])) { response->status=400;error(response,id,-32600,"Initialize requires an ID and no existing session.");goto done; }
        client=session(s,NULL,1);
        if (!client) { response->status=503;error(response,id,-32603,"The MCP session limit was reached.");goto done; }
        memcpy(response->session,client->id,sizeof(response->session));
        char *requested=string(&request,field(&request,params,L"protocolVersion"));
        const char *selected=requested && protocol_supported(requested)?requested:"2025-11-25";
        Buffer b={0};text(&b,"{\"protocolVersion\":");quote(&b,selected);
        text(&b,",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"appsandbox\",\"version\":");quote(&b,s->version);
        text(&b,"},\"instructions\":");quote(&b,instructions);text(&b,"}");result=finish(&b);free(requested);
    } else {
        client=session(s,session_id,0);
        if (!client) { response->status=session_id && session_id[0]?404:400;error(response,id,-32600,"An initialized MCP session is required.");goto done; }
        if (!id) {
            if (!strcmp(method,"notifications/initialized")) { lock(s);client->ready=1;unlock(s); }
            response->status=202;goto done;
        }
        lock(s);int ready=client->ready;unlock(s);
        if (!strcmp(method,"ping")) { Buffer b={0};text(&b,"{}");result=finish(&b); }
        else if (!ready) error(response,id,-32600,"Send notifications/initialized before using tools.");
        else if (!strcmp(method,"tools/list")) result=tools(s);
        else if (!strcmp(method,"tools/call")) result=call_tool(s,client,&request,params);
        else error(response,id,-32601,"Method not found.");
    }
    if (result) {
        Buffer b={0};text(&b,"{\"jsonrpc\":\"2.0\",\"id\":");text(&b,id);text(&b,",\"result\":");text(&b,result);text(&b,"}");response->json=finish(&b);
    } else if (!response->json) error(response,id,-32603,"MCP could not complete the request.");
done:
    if (client) { lock(s);--client->references;unlock(s); }
    free(id);free(method);free(result);dispose(&request);
}
