#pragma once
#include "asb_vm_id.h"

typedef uint64_t AsbTemplateId;
#define ASB_TEMPLATE_ID_TEXT_SIZE 26

static inline void asb_template_id_format(AsbTemplateId id,char out[ASB_TEMPLATE_ID_TEXT_SIZE])
{
    char vm[ASB_VM_ID_TEXT_SIZE];
    asb_vm_id_format(id,vm);
    memcpy(out,"template-",9);
    memcpy(out+9,vm+3,17);
}

static inline AsbTemplateId asb_template_id_parse(const char *text)
{
    char vm[ASB_VM_ID_TEXT_SIZE];
    if (!text || strlen(text)!=25 || memcmp(text,"template-",9)) return 0;
    memcpy(vm,"vm-",3);memcpy(vm+3,text+9,17);
    return asb_vm_id_parse(vm);
}

static inline void asb_template_id_format_w(AsbTemplateId id,wchar_t out[ASB_TEMPLATE_ID_TEXT_SIZE])
{
    char text[ASB_TEMPLATE_ID_TEXT_SIZE];
    asb_template_id_format(id,text);
    for (int i=0;i<ASB_TEMPLATE_ID_TEXT_SIZE;++i) out[i]=text[i];
}

static inline AsbTemplateId asb_template_id_parse_w(const wchar_t *text)
{
    char narrow[ASB_TEMPLATE_ID_TEXT_SIZE];
    if (!text || wcslen(text)!=25) return 0;
    for (int i=0;i<ASB_TEMPLATE_ID_TEXT_SIZE;++i) {
        if ((unsigned)text[i]>127) return 0;
        narrow[i]=(char)text[i];
    }
    return asb_template_id_parse(narrow);
}

static inline void asb_resource_id_format(AsbVmId id,int is_template,char out[ASB_TEMPLATE_ID_TEXT_SIZE])
{
    if (is_template) asb_template_id_format(id,out);
    else asb_vm_id_format(id,out);
}

static inline void asb_resource_id_format_w(AsbVmId id,int is_template,wchar_t out[ASB_TEMPLATE_ID_TEXT_SIZE])
{
    if (is_template) asb_template_id_format_w(id,out);
    else asb_vm_id_format_w(id,out);
}

static inline AsbVmId asb_resource_id_load(const char *text,int is_template,int *migrated)
{
    AsbVmId id=is_template?asb_template_id_parse(text):asb_vm_id_parse(text);
    if (!id && is_template) {
        id=asb_vm_id_parse(text);
        if (id) *migrated=1;
    }
    return id;
}

static inline AsbVmId asb_resource_id_load_w(const wchar_t *text,int is_template,int *migrated)
{
    char narrow[ASB_TEMPLATE_ID_TEXT_SIZE];
    if (!text || wcslen(text)>=ASB_TEMPLATE_ID_TEXT_SIZE) return 0;
    size_t i;
    for (i=0;text[i];++i) {
        if ((unsigned)text[i]>127) return 0;
        narrow[i]=(char)text[i];
    }
    narrow[i]=0;
    return asb_resource_id_load(narrow,is_template,migrated);
}
