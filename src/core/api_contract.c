#include "api_contract.h"
#include "api_operations.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

static char *write_string(char *out,const char *text)
{
    static const char hex[]="0123456789abcdef";
    *out++='"';
    for (const unsigned char *p=(const unsigned char *)text;*p;++p) {
        if (*p=='"' || *p=='\\') { *out++='\\';*out++=(char)*p; }
        else if (*p<32) {
            memcpy(out,"\\u00",4);out+=4;*out++=hex[*p>>4];*out++=hex[*p&15];
        } else *out++=(char)*p;
    }
    *out++='"';return out;
}

static int input_integer(const ApiJson *json,int object,const wchar_t *key,int *value,int fallback,int required)
{
    int token=api_json_field(json,object,key);
    if (token==-1 && !required) { *value=fallback;return 1; }
    return api_json_int(json,token,value);
}

static int input_boolean(const ApiJson *json,int object,const wchar_t *key,int *value,int fallback)
{
    int token=api_json_field(json,object,key);
    if (token==-1) { *value=fallback;return 1; }
    return api_json_bool(json,token,value);
}

uint32_t asb_api_parse_input(const ApiJson *json,AsbApiInputAction actions[128],int *count)
{
    int array=api_json_field(json,0,L"actions"),total=0;*count=0;
    if (array<0 || json->tokens[array].type!=API_JSON_ARRAY) return 87;
    for (int token=array+1;token<json->tokens[array].next;token=json->tokens[token].next) {
        if (*count>=128 || json->tokens[token].type!=API_JSON_OBJECT) return 87;
        AsbApiInputAction *a=&actions[(*count)++];wchar_t type[32];
        if (!api_json_string(json,api_json_field(json,token,L"type"),type,32)) return 87;
        if (!wcscmp(type,L"move") || !wcscmp(type,L"click")) {
            a->type=!wcscmp(type,L"move")?INPUT_MOUSE_MOVE:ASB_API_CLICK;
            if (!input_integer(json,token,L"x",&a->x,0,1) || !input_integer(json,token,L"y",&a->y,0,1) ||
                !input_integer(json,token,L"width",&a->width,0,1) || !input_integer(json,token,L"height",&a->height,0,1) ||
                !input_integer(json,token,L"button",&a->button,0,0) || a->width<2 || a->width>7680 ||
                a->height<2 || a->height>4320 || a->x<0 || a->x>=a->width || a->y<0 || a->y>=a->height || a->button<0 || a->button>2) return 87;
        } else if (!wcscmp(type,L"text") || !wcscmp(type,L"unicode")) {
            a->type=!wcscmp(type,L"unicode")?INPUT_UNICODE:ASB_API_TEXT;
            a->text=calloc(8193,sizeof(wchar_t));if (!a->text) return 8;
            if (!api_json_string(json,api_json_field(json,token,L"text"),a->text,8193)) return 87;
            int length=0;
            for (const wchar_t *p=a->text;*p;++p,++length) {
                if (*p>=0xd800 && *p<=0xdbff) { ++p;if (*p<0xdc00 || *p>0xdfff) return 87; }
                else if (*p>=0xdc00 && *p<=0xdfff) return 87;
            }
            if (length>4096 || (total+=length)>4096) return 87;
        } else if (!wcscmp(type,L"key")) {
            a->type=INPUT_KEY_PHYSICAL;
            if (!input_integer(json,token,L"scan",&a->scan,0,1) || a->scan<1 || a->scan>255 ||
                !input_boolean(json,token,L"down",&a->down,1) || !input_boolean(json,token,L"extended",&a->extended,0)) return 87;
        } else if (!wcscmp(type,L"button")) {
            a->type=INPUT_MOUSE_BUTTON;
            if (!input_integer(json,token,L"button",&a->button,0,0) || a->button<0 || a->button>2 ||
                !input_boolean(json,token,L"down",&a->down,1)) return 87;
        } else if (!wcscmp(type,L"scroll")) {
            a->type=INPUT_MOUSE_WHEEL;
            if (!input_integer(json,token,L"delta",&a->delta,0,1) || a->delta < -12000 || a->delta>12000) return 87;
        } else return 87;
    }
    return *count?0:87;
}

static uint32_t input_send(void *context,const AsbApiInputOps *ops,uint32_t type,uint32_t a,uint32_t b,uint32_t c)
{
    InputPacket packet={INPUT_MAGIC,type,a,b,c};return ops->send(context,&packet);
}

uint32_t asb_api_perform_input(const AsbApiInputAction *actions,int count,void *context,const AsbApiInputOps *ops)
{
    uint32_t error=0;
    for (int i=0;i<count && !error;++i) {
        const AsbApiInputAction *a=&actions[i];
        if (a->type==INPUT_MOUSE_MOVE || a->type==ASB_API_CLICK) {
            uint32_t x=(uint32_t)(((uint64_t)a->x*65535+(a->width-1)/2)/(a->width-1));
            uint32_t y=(uint32_t)(((uint64_t)a->y*65535+(a->height-1)/2)/(a->height-1));
            error=input_send(context,ops,INPUT_MOUSE_MOVE,x,y,INPUT_MOUSE_NORMALIZED);
            if (!error && a->type==ASB_API_CLICK) {
                error=input_send(context,ops,INPUT_MOUSE_BUTTON,(uint32_t)a->button,1,0);ops->sleep(40);
                if (!error) error=input_send(context,ops,INPUT_MOUSE_BUTTON,(uint32_t)a->button,0,0);
            }
        } else if (a->type==ASB_API_TEXT || a->type==INPUT_UNICODE) {
            unsigned sent=0;
            for (const wchar_t *p=a->text;*p && !error;++p) {
                uint32_t scalar=(uint32_t)*p;
                if (a->type==INPUT_UNICODE) {
                    if (scalar>=0xd800 && scalar<=0xdbff) scalar=0x10000+((scalar-0xd800)<<10)+((uint32_t)*++p-0xdc00);
                    error=input_send(context,ops,INPUT_UNICODE,scalar,0,0);
                } else {
                    if (scalar=='\r' && p[1]=='\n') { scalar='\n';++p; }
                    error=ops->text(context,scalar);
                }
                ops->sleep(20);
                if (!error && !(++sent%32)) error=ops->barrier(context);
            }
        } else if (a->type==INPUT_KEY_PHYSICAL)
            error=input_send(context,ops,a->type,0,(uint32_t)a->scan,(a->extended?INPUT_KEY_EXTENDED:0)|(a->down?0:INPUT_KEY_UP));
        else if (a->type==INPUT_MOUSE_BUTTON) error=input_send(context,ops,a->type,(uint32_t)a->button,(uint32_t)a->down,0);
        else error=input_send(context,ops,a->type,(uint32_t)a->delta,0,0);
        if (!error) error=ops->barrier(context);
    }
    return error;
}

char *asb_api_error_json(const char *code,const char *message)
{
    if (!code || !message) return NULL;
    size_t a=strlen(code),b=strlen(message);
    if (a>(SIZE_MAX-64)/6 || b>(SIZE_MAX-64)/6-a) return NULL;
    char *json=malloc((a+b)*6+64),*out=json;
    if (!json) return NULL;
    memcpy(out,"{\"error\":{\"code\":",17);out+=17;
    out=write_string(out,code);
    memcpy(out,",\"message\":",11);out+=11;
    out=write_string(out,message);
    memcpy(out,"}}",3);return json;
}

AsbApiOperationError asb_api_operation_error(uint32_t error,int input_operation)
{
    switch (error) {
    case 87:case 13:return (AsbApiOperationError){400,"invalid_arg","Invalid operation arguments or guest response."};
    case 5:return input_operation?
        (AsbApiOperationError){409,"user_has_control","The user has control. Stop input and wait for Return control; do not retry input automatically."}:
        (AsbApiOperationError){403,"permission_denied","The guest has not granted access to the requested feature."};
    case 1223:return (AsbApiOperationError){410,"agent_cancelled","The user stopped this work. Finish it and do not start it again without a new user instruction."};
    case 1460:case 258:return (AsbApiOperationError){504,"timeout","The operation timed out waiting for the guest."};
    case 21:case 170:return (AsbApiOperationError){409,"not_ready","The VM input channel or requested operation is not ready."};
    case 1168:return (AsbApiOperationError){404,"not_found","The requested session was not found for this VM."};
    case 50:return (AsbApiOperationError){501,"not_supported","This text cannot be represented with the host keyboard layout. Use physical key actions."};
    case 120:return (AsbApiOperationError){501,"not_supported","The requested feature is not supported for this guest platform."};
    default:return (AsbApiOperationError){500,"automation_failed","The host could not complete the operation."};
    }
}

static int path_property(const wchar_t *pattern,const wchar_t *key)
{
    wchar_t placeholder[132];
    size_t length=wcslen(key);
    if (!pattern || length>128) return 0;
    placeholder[0]='{';wmemcpy(placeholder+1,key,length);placeholder[length+1]='}';placeholder[length+2]=0;
    return wcsstr(pattern,placeholder)!=NULL;
}

static int match_path(const wchar_t *pattern,const wchar_t *path)
{
    while (*pattern && *path) {
        if (*pattern!='{') { if (*pattern++!=*path++) return 0;continue; }
        const wchar_t *end=wcschr(pattern,'}');
        if (!end || *path=='/') return 0;
        while (*path && *path!='/') ++path;
        pattern=end+1;
    }
    return !*pattern && !*path;
}

static int schema_valid(const ApiJson *catalog,int schema,const ApiJson *request,int value,const wchar_t *pattern)
{
    if (schema<0 || value<0) return 0;
    wchar_t type[16]={0};
    api_json_string(catalog,api_json_field(catalog,schema,L"type"),type,16);
    int actual=request->tokens[value].type,number=0,minimum,maximum;
    if (!wcscmp(type,L"integer")) {
        if (!api_json_int(request,value,&number)) return 0;
        if (api_json_int(catalog,api_json_field(catalog,schema,L"minimum"),&minimum) && number<minimum) return 0;
        if (api_json_int(catalog,api_json_field(catalog,schema,L"maximum"),&maximum) && number>maximum) return 0;
    } else if (!wcscmp(type,L"boolean")) {
        if (!api_json_bool(request,value,&number)) return 0;
    } else if (!wcscmp(type,L"string") && actual!=API_JSON_STRING) return 0;
    else if (!wcscmp(type,L"object") && actual!=API_JSON_OBJECT) return 0;
    else if (!wcscmp(type,L"array") && actual!=API_JSON_ARRAY) return 0;
    int enumeration=api_json_field(catalog,schema,L"enum");
    if (actual==API_JSON_STRING) {
        size_t capacity=(size_t)(request->tokens[value].end-request->tokens[value].start)+1;
        wchar_t *text=calloc(capacity,sizeof(*text));
        if (!text || !api_json_string(request,value,text,capacity)) { free(text);return 0; }
        int length=0;
        for (size_t i=0;text[i];++i,++length) if (text[i]>=0xd800 && text[i]<=0xdbff) ++i;
        int valid=(!api_json_int(catalog,api_json_field(catalog,schema,L"minLength"),&minimum) || length>=minimum) &&
            (!api_json_int(catalog,api_json_field(catalog,schema,L"maxLength"),&maximum) || length<=maximum);
        if (enumeration>=0) {
            int found=0;wchar_t candidate[128];
            for (int i=enumeration+1;i<catalog->tokens[enumeration].next;i=catalog->tokens[i].next)
                if (api_json_string(catalog,i,candidate,128) && !wcscmp(text,candidate)) { found=1;break; }
            valid=valid && found;
        }
        free(text);if (!valid) return 0;
    } else if (enumeration>=0) return 0;
    if (actual==API_JSON_OBJECT) {
        int properties=api_json_field(catalog,schema,L"properties");
        for (int key=value+1;key<request->tokens[value].next;) {
            wchar_t name[128];
            if (!api_json_string(request,key,name,128) || path_property(pattern,name)) return 0;
            int property=api_json_field(catalog,properties,name),item=request->tokens[key].next;
            if (property<0 || !schema_valid(catalog,property,request,item,NULL)) return 0;
            key=request->tokens[item].next;
        }
        int required=api_json_field(catalog,schema,L"required");
        if (required>=0) for (int i=required+1;i<catalog->tokens[required].next;i=catalog->tokens[i].next) {
            wchar_t name[128];
            if (!api_json_string(catalog,i,name,128)) return 0;
            if (!path_property(pattern,name) && api_json_field(request,value,name)<0) return 0;
        }
    } else if (actual==API_JSON_ARRAY) {
        int count=0,items=api_json_field(catalog,schema,L"items");
        for (int i=value+1;i<request->tokens[value].next;i=request->tokens[i].next) {
            if (items<0 || !schema_valid(catalog,items,request,i,NULL)) return 0;
            ++count;
        }
        if (api_json_int(catalog,api_json_field(catalog,schema,L"minItems"),&minimum) && count<minimum) return 0;
        if (api_json_int(catalog,api_json_field(catalog,schema,L"maxItems"),&maximum) && count>maximum) return 0;
    }
    return 1;
}

int asb_api_validate_request(const char *method,const char *path,const wchar_t *body)
{
    if (!method || !path || !asb_api_validate_body(body)) return 0;
    if (strncmp(path,"/v1/",4)) return 1;
    size_t length=strcspn(path+4,"?"),prefix=0;
    wchar_t route[2048];
    const char *tail=path+4;
    if (!strncmp(tail,"vms/",4)) { wmemcpy(route,L"/vm-ids/",8);prefix=8;tail+=4;length-=4; }
    else if (!strncmp(tail,"templates/",10)) { wmemcpy(route,L"/template-ids/",14);prefix=14;tail+=10;length-=10; }
    else { route[0]='/';prefix=1; }
    if (length>=2048-prefix) return 0;
    for (size_t i=0;i<length;++i) route[prefix+i]=(unsigned char)tail[i];
    route[prefix+length]=0;
    const char *source=asb_api_operations_json();
    size_t size=strlen(source)+1;
    wchar_t *wide=calloc(size,sizeof(*wide));
    ApiJsonToken *tokens=calloc(8192,sizeof(*tokens)),*values=calloc(8192,sizeof(*values));
    ApiJson catalog,request;int valid=wide && tokens && values;
    if (valid) {
        for (size_t i=0;i<size;++i) wide[i]=(unsigned char)source[i];
        valid=api_json_parse(&catalog,wide,tokens,8192) && api_json_parse(&request,body && *body?body:L"{}",values,8192);
    }
    if (valid) {
        int array=api_json_field(&catalog,0,L"operations");
        for (int i=array+1;i<tokens[array].next;i=tokens[i].next) {
            wchar_t pattern[256],verb[16];
            if (!api_json_string(&catalog,api_json_field(&catalog,i,L"path"),pattern,256) ||
                !api_json_string(&catalog,api_json_field(&catalog,i,L"method"),verb,16)) { valid=0;break; }
            wchar_t expected[16];size_t n=strlen(method);
            if (n>=16) { valid=0;break; }
            for (size_t j=0;j<=n;++j) expected[j]=(unsigned char)method[j];
            if (wcscmp(verb,expected) || !match_path(pattern,route)) continue;
            valid=schema_valid(&catalog,api_json_field(&catalog,i,L"inputSchema"),&request,0,pattern);
            wchar_t operation[64];
            if (valid && api_json_string(&catalog,api_json_field(&catalog,i,L"name"),operation,64) && !wcscmp(operation,L"input")) {
                AsbApiInputAction actions[128]={0};int count=0;
                valid=asb_api_parse_input(&request,actions,&count)==0;
                for (int action=0;action<count;++action) free(actions[action].text);
            }
            break;
        }
    }
    free(wide);free(tokens);free(values);return valid;
}

typedef struct { const wchar_t *name;int type;size_t limit; } Field;

static const Field fields[]={
    {L"ramMb",API_JSON_NUMBER,0},{L"hddGb",API_JSON_NUMBER,0},
    {L"cpuCores",API_JSON_NUMBER,0},{L"gpuMode",API_JSON_NUMBER,0},
    {L"networkMode",API_JSON_NUMBER,0},{L"snapIndex",API_JSON_NUMBER,0},
    {L"branchIndex",API_JSON_NUMBER,0},{L"timeoutMs",API_JSON_NUMBER,0},
    {L"sshEnabled",API_JSON_TRUE,0},{L"sshDeployKey",API_JSON_TRUE,0},
    {L"testMode",API_JSON_TRUE,0},{L"isTemplate",API_JSON_TRUE,0},
    {L"openDisplay",API_JSON_TRUE,0},{L"force",API_JSON_TRUE,0},
    {L"closeStdin",API_JSON_TRUE,0},
    {L"name",API_JSON_STRING,255},{L"osType",API_JSON_STRING,31},
    {L"imagePath",API_JSON_STRING,259},{L"diskDirectory",API_JSON_STRING,259},
    {L"templateName",API_JSON_STRING,255},{L"templateId",API_JSON_STRING,25},
    {L"adminUser",API_JSON_STRING,127},{L"adminPass",API_JSON_STRING,255},
    {L"gpuId",API_JSON_STRING,511},{L"netAdapter",API_JSON_STRING,255},
    {L"branchName",API_JSON_STRING,127}
};

int asb_api_validate_body(const wchar_t *body)
{
    ApiJson json;ApiJsonToken *tokens=calloc(8192,sizeof(*tokens));
    if (!tokens) return 0;
    int valid=api_json_parse(&json,body && body[0]?body:L"{}",tokens,8192) &&
        tokens[0].type==API_JSON_OBJECT;
    wchar_t *decoded=NULL;
    if (valid) {
        size_t length=wcslen(json.text);
        decoded=calloc(length+1,sizeof(*decoded));
        if (!decoded) valid=0;
        for (int i=0;valid && i<json.count;++i) {
            if (tokens[i].type!=API_JSON_STRING) continue;
            if (!api_json_string(&json,i,decoded,length+1)) { valid=0;break; }
            for (size_t p=0;decoded[p];++p) {
                unsigned unit=(unsigned)decoded[p];
                if (unit>=0xd800 && unit<=0xdbff) {
                    unsigned low=(unsigned)decoded[++p];
                    if (low<0xdc00 || low>0xdfff) { valid=0;break; }
                } else if (unit>=0xdc00 && unit<=0xdfff) { valid=0;break; }
            }
        }
        for (int object=0;valid && object<json.count;++object) {
            if (tokens[object].type!=API_JSON_OBJECT) continue;
            for (int key=object+1;valid && key<tokens[object].next;) {
                if (!api_json_string(&json,key,decoded,length+1) ||
                    api_json_field(&json,object,decoded)<0) { valid=0;break; }
                key=tokens[tokens[key].next].next;
            }
        }
    }
    for (size_t i=0;valid && i<sizeof(fields)/sizeof(*fields);++i) {
        int token=api_json_field(&json,0,fields[i].name),number;
        if (token==-1) continue;
        if (token<0) { valid=0;break; }
        if (fields[i].type==API_JSON_NUMBER) valid=api_json_int(&json,token,&number);
        else if (fields[i].type==API_JSON_TRUE) valid=api_json_bool(&json,token,&number);
        else valid=api_json_string(&json,token,decoded,fields[i].limit+1);
    }
    free(decoded);free(tokens);return valid;
}

int asb_api_validate_start(const wchar_t *body)
{
    if (!asb_api_validate_body(body)) return 0;
    int snapshot=-1,branch=-1;
    api_json_get_int(body,L"snapIndex",&snapshot);
    api_json_get_int(body,L"branchIndex",&branch);
    wchar_t name[128]={0};
    api_json_get_string(body,L"branchName",name,128);
    return asb_api_validate_start_options(snapshot,branch,name[0]!=0);
}

int asb_api_validate_start_options(int snapshot,int branch,int named_branch)
{
    if (snapshot < -2 || branch < -1) return 0;
    if ((branch>=0 || named_branch) && snapshot==-1) return 0;
    if (branch>=0 && named_branch) return 0;
    return 1;
}

const wchar_t *asb_api_headless_route(const char *method,const wchar_t *path,uint64_t *session_id)
{
    int get=!strcmp(method,"GET"),post=!strcmp(method,"POST"),del=!strcmp(method,"DELETE");
    *session_id=0;
    if (post && !wcscmp(path,L"screenshot")) return L"screenshot";
    if (!wcscmp(path,L"input")) return get?L"input_status":post?L"input":NULL;
    if (!wcscmp(path,L"input/control")) return post?L"request_control":del?L"finish":NULL;
    if (post && !wcscmp(path,L"input/wait")) return L"wait_control";
    if (post && !wcscmp(path,L"ssh/sessions")) return L"ssh_open";
    if (!wcsncmp(path,L"ssh/sessions/ssh-",17) && wcslen(path)>=33) {
        uint64_t id=0;
        for (int i=17;i<33;++i) {
            unsigned c=(unsigned)path[i];
            if (c>='0' && c<='9') c-='0';
            else if (c>='a' && c<='f') c-='a'-10;
            else return NULL;
            id=(id<<4)|c;
        }
        if (!id) return NULL;
        const wchar_t *operation=!path[33]?(get?L"ssh_read":post?L"ssh_write":del?L"ssh_close":NULL):
            post && !wcscmp(path+33,L"/read")?L"ssh_read":NULL;
        if (operation) *session_id=id;
        return operation;
    }
    return NULL;
}

static int route_index(const wchar_t *start,const wchar_t **end,int *out)
{
    if (!start[0] || (start[0]!=L'-' && (start[0]<L'0' || start[0]>L'9'))) return 0;
    wchar_t *after;
    errno=0;
    long value=wcstol(start,&after,10);
    if (errno || after==start || value<INT_MIN || value>INT_MAX || (*after && *after!=L'/')) return 0;
    *end=after;*out=(int)value;return 1;
}

int asb_api_snapshot_route(const char *method,const wchar_t *path,int *snapshot,int *branch)
{
    *snapshot=-1;*branch=-1;
    if (!wcscmp(path,L"snapshots")) return !strcmp(method,"GET")?1:!strcmp(method,"POST")?2:0;
    if (wcsncmp(path,L"snapshots/",10)) return 0;
    const wchar_t *end;
    if (!route_index(path+10,&end,snapshot) || (*snapshot<0 && *snapshot!=-2)) return -1;
    if (!*end) {
        if (!strcmp(method,"DELETE")) return *snapshot>=0?3:-1;
        return !strcmp(method,"PUT")?4:0;
    }
    if (wcsncmp(end,L"/branches/",10) || !route_index(end+10,&end,branch) || *end || *branch<0) return -1;
    return !strcmp(method,"DELETE")?5:0;
}

const char *asb_api_capabilities_json(int windows_host)
{
    return windows_host?
        "{\"snapshots\":true,\"templates\":true,\"screenshots\":true,\"input\":true,\"sshSessions\":true,\"gpuSelection\":true,"
        "\"guestOs\":[\"Windows\",\"Linux\"],\"templateGuestOs\":[\"Windows\"],\"networkModes\":[0,1,2,3]}":
        "{\"snapshots\":false,\"templates\":true,\"screenshots\":true,\"input\":true,\"sshSessions\":true,\"gpuSelection\":false,"
        "\"guestOs\":[\"Windows\",\"macOS\"],\"templateGuestOs\":[\"Windows\",\"macOS\"],\"networkModes\":[1]}";
}

uint32_t asb_api_guest_support(int windows_host,const wchar_t *os_type,int is_template)
{
    wchar_t lower[16];size_t length=os_type?wcslen(os_type):0;
    if (!length || length>=16) return 87;
    for (size_t i=0;i<=length;++i) lower[i]=os_type[i]>='A' && os_type[i]<='Z'?os_type[i]+('a'-'A'):os_type[i];
    if (!wcscmp(lower,L"windows")) return 0;
    if (!wcscmp(lower,L"linux")) return windows_host && !is_template?0:120;
    if (!wcscmp(lower,L"macos")) return windows_host?120:0;
    return 87;
}

const char *asb_api_vm_fields_json(void)
{
    return "[\"id\",\"name\",\"osType\",\"diskDirectory\",\"state\",\"running\","
        "\"agentOnline\",\"installComplete\",\"building\",\"progress\",\"sshState\","
        "\"sshPort\",\"ramMb\",\"hddGb\",\"cpuCores\",\"gpuMode\",\"networkMode\","
        "\"displayOpen\",\"gpuId\",\"gpuName\"]";
}
