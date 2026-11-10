#pragma once
#include <windows.h>

typedef struct AgentOverlay AgentOverlay;

AgentOverlay *agent_overlay_create(HWND render, HWND notify, UINT take_control_message,UINT64 vm_id);
void agent_overlay_destroy(AgentOverlay *overlay);
void agent_overlay_update(AgentOverlay *overlay, int state, int x, int y,
                          UINT frame_width, UINT frame_height, BOOL waiting, BOOL finished);
void agent_overlay_resize(AgentOverlay *overlay);
BOOL agent_overlay_hit_test(AgentOverlay *overlay, POINT point);
void agent_overlay_pointer_activity(AgentOverlay *overlay);
BOOL agent_overlay_mouse(AgentOverlay *overlay, UINT message, WPARAM keys, LPARAM position);
void agent_overlay_cancel_input(AgentOverlay *overlay);
UINT32 agent_overlay_work_id(AgentOverlay *overlay);

