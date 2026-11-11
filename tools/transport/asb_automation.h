#pragma once
#include <stdint.h>

#define ASB_CH_SCREENSHOT 8
#define ASB_SCREENSHOT_REQUEST_MAGIC 0x51525341u
#define ASB_SCREENSHOT_REPLY_MAGIC   0x50525341u
#define ASB_SCREENSHOT_VERSION 1u
#define ASB_SCREENSHOT_GPU_SHARED 1u
#define ASB_INPUT_AGENT_END 17u
#define ASB_INPUT_USER_GRANT 18u
#define ASB_INPUT_USER_REVOKE 19u
#define ASB_INPUT_USER_CANCEL 25u
#define ASB_STATUS_AGENT_HAS_CONTROL 1u
#define ASB_STATUS_AGENT_WAITING 2u
#define ASB_STATUS_USER_HAS_CONTROL 4u
#define ASB_STATUS_AGENT_FINISHED 8u
#define ASB_STATUS_USER_PAUSED 16u
#define ASB_STATUS_AGENT_CANCELLED 32u
#define ASB_CONTROL_AVAILABLE 0u
#define ASB_CONTROL_AGENT 1u
#define ASB_CONTROL_USER 2u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, version;
    uint64_t request_id;
    uint32_t timeout_ms, flags;
} AsbScreenshotRequest;

typedef struct {
    uint32_t magic, version;
    uint64_t request_id;
    uint32_t status, width, height, stride;
    uint64_t frame_seq, capture_time_ms;
    uint32_t flags, data_size;
} AsbScreenshotReply;
#pragma pack(pop)
