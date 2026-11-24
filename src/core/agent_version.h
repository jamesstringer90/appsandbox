#ifndef ASB_AGENT_VERSION_H
#define ASB_AGENT_VERSION_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASB_AGENT_VERSION_TEXT_SIZE 16

static inline uint32_t asb_agent_version_parse(const char *text)
{
    if (*text++ != 'v') return 0;
    uint32_t value = 0;
    for (int i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') return 0;
        char *end;
        unsigned long part = strtoul(text, &end, 10);
        if (part > 999 || *end != (i == 2 ? '\0' : '.')) return 0;
        value = value * 1000 + (uint32_t)part;
        text = end + 1;
    }
    return value;
}

static inline void asb_agent_version_format(uint32_t version, char out[ASB_AGENT_VERSION_TEXT_SIZE])
{
    out[0] = '\0';
    if (version)
        snprintf(out, ASB_AGENT_VERSION_TEXT_SIZE, "v%u.%u.%u",
                 (unsigned)(version / 1000000), (unsigned)(version / 1000 % 1000),
                 (unsigned)(version % 1000));
}

static inline int asb_agent_version_read_hello(const char *line, uint32_t *version)
{
    *version = 0;
    if (!strcmp(line, "hello")) return 1;
    if (strncmp(line, "hello ", sizeof("hello ") - 1)) return 0;
    *version = asb_agent_version_parse(line + sizeof("hello ") - 1);
    return *version != 0;
}

#endif
