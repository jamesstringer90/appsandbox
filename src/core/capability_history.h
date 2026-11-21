#ifndef ASB_CAPABILITY_HISTORY_H
#define ASB_CAPABILITY_HISTORY_H

#define ASB_CAPABILITY_HISTORY(ENTRY) \
    ENTRY("agent.idd_status", "Windows guest on Windows", "v0.1.0", "5db568e") \
    ENTRY("agent.idd_status", "Ubuntu guest on Windows", "v0.1.3", "889ccbc") \
    ENTRY("agent.idd_status", "Windows guest on macOS", "v0.1.4", "0d4897d") \
    ENTRY("agent.sas", "Windows guest", "v0.1.13", "372c9be") \
    ENTRY("agent.display_monitor", "Windows guest", "v0.1.13", "35ece9b") \
    ENTRY("agent.disk_resize", "Windows, Ubuntu and macOS guests", "unreleased", "") \
    \
    ENTRY("viewer.fullscreen", "Windows and macOS hosts", "v0.1.13", "c059afb") \
    ENTRY("input.keyboard.v2", "Windows and Ubuntu guests", "v0.1.5", "4e2493d") \
    ENTRY("input.keyboard.v2", "Native macOS automation", "v0.1.12", "4586f7c") \
    ENTRY("input.mouse.v1", "Windows and Ubuntu guests", "v0.1.5", "9d6342f") \
    ENTRY("input.mouse.position", "Windows and Ubuntu guests", "v0.1.5", "9d6342f") \
    ENTRY("input.mouse.v2", "Windows and Ubuntu guests", "v0.1.10", "f985b39") \
    ENTRY("input.mouse.v2", "Native macOS automation", "v0.1.12", "4586f7c") \
    ENTRY("input.unicode", "Windows guest", "v0.1.12", "e188f32") \
    \
    ENTRY("DISPLAY_GPU_CAP_RESIZE", "Windows guest", "v0.1.10", "3cd3ccc") \
    ENTRY("DISPLAY_GPU_CAP_RESIZE", "Ubuntu guest on Windows", "v0.1.10", "f985b39") \
    ENTRY("DISPLAY_GPU_CAP_IVSHMEM", "Windows guest on macOS", "v0.1.10", "3cd3ccc") \
    ENTRY("DISPLAY_GPU_CAP_FENCE", "Ubuntu guest on Windows", "v0.1.10", "140c811") \
    ENTRY("DISPLAY_GPU_CAP_REFRESH", "Windows and Ubuntu guests on Windows", "v0.1.10", "bf0af15") \
    ENTRY("DISPLAY_GPU_CAP_SNAPSHOT", "Windows guest on Windows", "v0.1.12", "6e025c2") \
    \
    ENTRY("ASB_DRM_GPU_CAP_BUFFER_IMPORT", "Ubuntu guest", "v0.1.10", "140c811") \
    ENTRY("ASB_DRM_GPU_CAP_NATIVE_HANDLES", "Ubuntu guest", "v0.1.10", "140c811") \
    ENTRY("ASB_DRM_GPU_CAP_SYNC_FILE", "Ubuntu guest", "v0.1.10", "140c811") \
    ENTRY("ASB_DRM_GPU_CAP_PRESENT", "Ubuntu guest", "v0.1.10", "140c811") \
    ENTRY("ASB_DRM_GPU_CAP_SCANOUT", "Ubuntu guest", "v0.1.10", "140c811") \
    ENTRY("ASB_DRM_GPU_CAP_RESIZE", "Ubuntu guest", "v0.1.10", "f985b39") \
    ENTRY("ASB_DRM_GPU_CAP_RGB_FORMATS", "Ubuntu guest", "v0.1.12", "b9e48a6") \
    ENTRY("ASB_DRM_GPU_CAP_MODIFIERS", "Ubuntu guest", "v0.1.12", "b9e48a6") \
    \
    ENTRY("api.snapshots", "Windows host", "v0.1.3", "4087baa") \
    ENTRY("api.templates", "Windows host", "v0.1.3", "4087baa") \
    ENTRY("api.templates", "macOS host", "v0.1.10", "10b95e9") \
    ENTRY("api.screenshots", "Windows and macOS hosts", "v0.1.12", "e188f32") \
    ENTRY("api.input", "Windows and macOS hosts", "v0.1.12", "e188f32") \
    ENTRY("api.sshSessions", "Windows and macOS hosts", "v0.1.12", "e188f32") \
    ENTRY("api.gpuSelection", "Windows host", "v0.1.12", "e188f32") \
    ENTRY("api.guestOs", "Windows host (windows11, ubuntu2604lts)", "v0.1.12", "50df4f6") \
    ENTRY("api.guestOs", "macOS host (windows11, macos27)", "v0.1.12", "50df4f6") \
    ENTRY("api.guestOsNames", "Windows and macOS hosts", "v0.1.12", "50df4f6") \
    ENTRY("api.templateGuestOs", "Windows host (windows11)", "v0.1.12", "50df4f6") \
    ENTRY("api.templateGuestOs", "macOS host (windows11, macos27)", "v0.1.12", "50df4f6") \
    ENTRY("api.networkModes", "Windows host (0, 1, 2, 3)", "v0.1.12", "e188f32") \
    ENTRY("api.networkModes", "macOS host (1)", "v0.1.12", "e188f32") \
    ENTRY("api.diskResize", "Windows and macOS hosts", "unreleased", "") \
    ENTRY("mcp.tools", "Windows and macOS hosts", "v0.1.12", "e188f32")

#endif
