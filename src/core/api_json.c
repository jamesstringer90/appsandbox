#include "api_json.h"
#include <wchar.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

#define TRUE 1
#define FALSE 0
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))

static void space(ApiJson *j)
{
    while (j->text[j->pos] && wcschr(L" \t\n\r",j->text[j->pos])) ++j->pos;
}

static int hex(wchar_t c)
{
    return c>=L'0' && c<=L'9'?c-L'0':c>=L'a' && c<=L'f'?c-L'a'+10:c>=L'A' && c<=L'F'?c-L'A'+10:-1;
}

static int string_end(ApiJson *j)
{
    ++j->pos;
    while (j->text[j->pos] && j->text[j->pos]!=L'"') {
        wchar_t c=j->text[j->pos++];
        if (c<32) return FALSE;
        if (c==L'\\') {
            c=j->text[j->pos++];
            if (!c) return FALSE;
            if (c==L'u') {
                for (int i=0;i<4;++i) if (hex(j->text[j->pos++])<0) return FALSE;
            } else if (!wcschr(L"\"\\/bfnrt",c)) return FALSE;
        }
    }
    return j->text[j->pos++]==L'"';
}

static int value(ApiJson *j,int depth)
{
    if (depth>32 || j->count>=j->capacity) return -1;
    space(j);
    int index=j->count++;
    ApiJsonToken *t=&j->tokens[index];
    t->start=j->pos;
    wchar_t c=j->text[j->pos];
    if (c==L'{' || c==L'[') {
        t->type=c==L'{'?API_JSON_OBJECT:API_JSON_ARRAY;
        wchar_t end=c==L'{'?L'}':L']';
        ++j->pos;
        space(j);
        if (j->text[j->pos]!=end) for (;;) {
            if (c==L'{') {
                int key=value(j,depth+1);
                if (key<0 || j->tokens[key].type!=API_JSON_STRING) return -1;
                space(j);
                if (j->text[j->pos++]!=L':') return -1;
            }
            if (value(j,depth+1)<0) return -1;
            space(j);
            if (j->text[j->pos]!=L',') break;
            ++j->pos;
        }
        if (j->text[j->pos++]!=end) return -1;
    } else if (c==L'"') {
        t->type=API_JSON_STRING;
        if (!string_end(j)) return -1;
    } else if (c==L'-' || (c>=L'0' && c<=L'9')) {
        t->type=API_JSON_NUMBER;
        if (c==L'-') ++j->pos;
        if (j->text[j->pos]==L'0') ++j->pos;
        else {
            if (j->text[j->pos]<L'1' || j->text[j->pos]>L'9') return -1;
            while (j->text[j->pos]>=L'0' && j->text[j->pos]<=L'9') ++j->pos;
        }
        if (j->text[j->pos]==L'.') {
            ++j->pos;
            if (j->text[j->pos]<L'0' || j->text[j->pos]>L'9') return -1;
            while (j->text[j->pos]>=L'0' && j->text[j->pos]<=L'9') ++j->pos;
        }
        if (j->text[j->pos]==L'e' || j->text[j->pos]==L'E') {
            ++j->pos;
            if (j->text[j->pos]==L'+' || j->text[j->pos]==L'-') ++j->pos;
            if (j->text[j->pos]<L'0' || j->text[j->pos]>L'9') return -1;
            while (j->text[j->pos]>=L'0' && j->text[j->pos]<=L'9') ++j->pos;
        }
    } else {
        const wchar_t *word=c==L't'?L"true":c==L'f'?L"false":L"null";
        t->type=c==L't'?API_JSON_TRUE:c==L'f'?API_JSON_FALSE:API_JSON_NULL;
        size_t n=wcslen(word);
        if (wcsncmp(j->text+j->pos,word,n)) return -1;
        j->pos+=(int)n;
    }
    t->end=j->pos;
    t->next=j->count;
    return index;
}

int api_json_parse(ApiJson *j,const wchar_t *text,ApiJsonToken *tokens,int capacity)
{
    *j=(ApiJson){text,tokens,0,capacity,0};
    if (value(j,0)!=0) return FALSE;
    space(j);
    return !text[j->pos];
}

int api_json_string(const ApiJson *j,int token,wchar_t *out,size_t capacity)
{
    if (token<0 || token>=j->count || j->tokens[token].type!=API_JSON_STRING || !capacity) return FALSE;
    const ApiJsonToken *t=&j->tokens[token];
    size_t n=0;
    for (int p=t->start+1;p<t->end-1;) {
        wchar_t c=j->text[p++];
        if (c==L'\\') {
            c=j->text[p++];
            if (c==L'u') {
                unsigned unit=0;
                for (int i=0;i<4;++i) unit=(unit<<4)|(unsigned)hex(j->text[p++]);
                c=(wchar_t)unit;
            } else {
                const wchar_t *esc=wcschr(L"\"\\/bfnrt",c);
                if (!esc) return FALSE;
                c=L"\"\\/\b\f\n\r\t"[esc-L"\"\\/bfnrt"];
            }
        }
        if (!c || n+1>=capacity) return FALSE;
        out[n++]=c;
    }
    out[n]=0;
    return TRUE;
}

int api_json_field(const ApiJson *j,int object,const wchar_t *key)
{
    if (object<0 || object>=j->count || j->tokens[object].type!=API_JSON_OBJECT) return -1;
    wchar_t name[128];
    int found=-1;
    for (int i=object+1;i<j->tokens[object].next;) {
        int field=i;
        i=j->tokens[i].next;
        if (i>=j->tokens[object].next) return -1;
        if (api_json_string(j,field,name,ARRAYSIZE(name)) && !wcscmp(name,key)) {
            if (found>=0) return -2;
            found=i;
        }
        i=j->tokens[i].next;
    }
    return found;
}

int api_json_int(const ApiJson *j,int token,int *out)
{
    if (token<0 || token>=j->count || j->tokens[token].type!=API_JSON_NUMBER) return FALSE;
    wchar_t *end;
    errno=0;
    long n=wcstol(j->text+j->tokens[token].start,&end,10);
    if (errno || end!=j->text+j->tokens[token].end || n<INT_MIN || n>INT_MAX) return FALSE;
    *out=(int)n;
    return TRUE;
}

int api_json_bool(const ApiJson *j,int token,int *out)
{
    if (token<0 || token>=j->count || (j->tokens[token].type!=API_JSON_TRUE && j->tokens[token].type!=API_JSON_FALSE)) return FALSE;
    *out=j->tokens[token].type==API_JSON_TRUE;
    return TRUE;
}

static int get_field(const wchar_t *text,const wchar_t *key,int type,void *out,size_t capacity)
{
    ApiJson json;ApiJsonToken *tokens=calloc(8192,sizeof(*tokens));
    if (!tokens) return 0;
    int result=0;
    if (api_json_parse(&json,text && text[0]?text:L"{}",tokens,8192)) {
        int token=api_json_field(&json,0,key);
        result=type==API_JSON_STRING?api_json_string(&json,token,out,capacity):
            type==API_JSON_NUMBER?api_json_int(&json,token,out):
            type==API_JSON_TRUE?api_json_bool(&json,token,out):token>=0;
    }
    free(tokens);return result;
}

int api_json_get_string(const wchar_t *text,const wchar_t *key,wchar_t *out,size_t capacity)
{
    return get_field(text,key,API_JSON_STRING,out,capacity);
}

int api_json_get_int(const wchar_t *text,const wchar_t *key,int *out)
{
    return get_field(text,key,API_JSON_NUMBER,out,0);
}

int api_json_get_bool(const wchar_t *text,const wchar_t *key,int *out)
{
    return get_field(text,key,API_JSON_TRUE,out,0);
}

int api_json_has_key(const wchar_t *text,const wchar_t *key)
{
    return get_field(text,key,0,NULL,0);
}
