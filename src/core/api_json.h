#pragma once
#include <stddef.h>
#include <wchar.h>

enum { API_JSON_OBJECT=1,API_JSON_ARRAY,API_JSON_STRING,API_JSON_NUMBER,API_JSON_TRUE,API_JSON_FALSE,API_JSON_NULL };
typedef struct { int type,start,end,next; } ApiJsonToken;
typedef struct { const wchar_t *text;ApiJsonToken *tokens;int count,capacity,pos; } ApiJson;
int api_json_parse(ApiJson *json,const wchar_t *text,ApiJsonToken *tokens,int capacity);
int api_json_field(const ApiJson *json,int object,const wchar_t *key);
int api_json_string(const ApiJson *json,int token,wchar_t *out,size_t capacity);
int api_json_int(const ApiJson *json,int token,int *value);
int api_json_bool(const ApiJson *json,int token,int *value);
int api_json_get_string(const wchar_t *text,const wchar_t *key,wchar_t *out,size_t capacity);
int api_json_get_int(const wchar_t *text,const wchar_t *key,int *out);
int api_json_get_bool(const wchar_t *text,const wchar_t *key,int *out);
int api_json_has_key(const wchar_t *text,const wchar_t *key);
