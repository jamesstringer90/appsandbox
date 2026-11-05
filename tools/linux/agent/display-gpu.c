#define CINTERFACE
#define COBJMACROS
#define INITGUID
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "display-gpu.h"
#include "asb_drm_uapi.h"
#include "d3dkmthk.h"

DEFINE_GUID(asb_factory_iid, 0x78ee5945, 0xc36e, 0x4b13, 0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06);
DEFINE_GUID(asb_list_iid, 0x526c7776, 0x40e9, 0x459b, 0xb7, 0x11, 0xf3, 0x2a, 0xd7, 0x6d, 0xfc, 0x28);
DEFINE_GUID(asb_adapter_iid, 0xf0db4c7f, 0xfe5a, 0x42a2, 0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e);
DEFINE_GUID(asb_graphics_attribute, 0x0c9ece4d, 0x2f6e, 0x4f01, 0x8c, 0x96, 0xe8, 0x9e, 0x33, 0x1b, 0x47, 0xb1);

struct dxcore_factory_vtable {
    IUnknownVtbl unknown;
    HRESULT (*CreateAdapterList)(void *, uint32_t, const GUID *, REFIID, void **);
};

struct dxcore_list_vtable {
    IUnknownVtbl unknown;
    HRESULT (*GetAdapter)(void *, uint32_t, REFIID, void **);
    uint32_t (*GetAdapterCount)(void *);
};

struct display_gpu_slot {
    ID3D12Resource *texture;
    ID3D12Resource *source;
    ID3D12Fence *fence;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *commands;
    uint64_t free_value;
    uint64_t copy_value;
    int free_fd;
};

struct display_gpu {
    int drm_fd;
    int dxg_fd;
    int event_fd;
    int copy_fd;
    void *d3d12_lib;
    void *dxcore_lib;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12Fence *complete;
    struct drm_asb_gpu_native native_complete;
    struct display_gpu_slot slots[DISPLAY_GPU_SLOTS];
    uint32_t next;
    uint32_t width;
    uint32_t height;
    uint64_t copied;
    uint64_t sequence;
    uint64_t adapter_luid;
    bool connected;
    ID3D12Resource *readback;
    void *pixels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    uint32_t readback_width;
    uint32_t readback_height;
};

static int gpu_ioctl(int fd, unsigned long command, void *args)
{
    int result;
    do result = ioctl(fd, command, args); while (result < 0 && errno == EINTR);
    return result;
}

static int gpu_wait(struct display_gpu *gpu, ID3D12Fence *fence, uint64_t value)
{
    struct timespec start, now, delay = {.tv_nsec = 1000000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        uint64_t completed = ID3D12Fence_GetCompletedValue(fence);
        if (completed == UINT64_MAX || FAILED(ID3D12Device_GetDeviceRemovedReason(gpu->device)))
            return -1;
        if (completed >= value) return 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) * 1000000000LL + now.tv_nsec - start.tv_nsec > 2000000000LL)
            return -1;
        nanosleep(&delay, NULL);
    }
}

static int gpu_native(struct display_gpu *gpu, ID3D12DeviceChild *object,
                      struct drm_asb_gpu_native *native)
{
    HANDLE handle = NULL;
    if (FAILED(ID3D12Device_CreateSharedHandle(gpu->device, object, NULL,
                                              GENERIC_ALL, NULL, &handle))) return -1;
    memset(native, 0, sizeof(*native));
    native->dxg_fd = gpu->dxg_fd;
    native->fd = (int)(uintptr_t)handle;
    if (gpu_ioctl(gpu->drm_fd, DRM_IOCTL_ASB_GPU_NATIVE, native) < 0) {
        close(native->fd);
        return -1;
    }
    return 0;
}

static int gpu_export(struct display_gpu *gpu, ID3D12DeviceChild *object, uint64_t *handle)
{
    struct drm_asb_gpu_native native;
    if (gpu_native(gpu, object, &native) < 0) return -1;
    struct d3dkmt_shareobjectwithhost share = {
        .device_handle.v = native.device_handle,
        .object_handle.v = native.object_handle,
    };
    int result = gpu_ioctl(gpu->dxg_fd, LX_DXSHAREOBJECTWITHHOST, &share);
    close(native.fd);
    if (result < 0 || !share.object_vail_nt_handle) return -1;
    *handle = share.object_vail_nt_handle;
    return 0;
}

static int gpu_transfer(int socket_fd, void *data, size_t size, bool sending)
{
    unsigned char *bytes = data;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (size) {
        struct pollfd pending = {.fd = socket_fd, .events = sending ? POLLOUT : POLLIN};
        int result = poll(&pending, 1, 100);
        if (result < 0 && errno != EINTR) return -1;
        if (result > 0) {
            ssize_t count = sending ? send(socket_fd, bytes, size, MSG_NOSIGNAL | MSG_DONTWAIT)
                                    : recv(socket_fd, bytes, size, MSG_DONTWAIT);
            if (!count) return -1;
            if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
            if (count > 0) { bytes += count; size -= (size_t)count; }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 10) return -1;
    }
    return 0;
}

void display_gpu_disconnect(struct display_gpu *gpu)
{
    if (!gpu) return;
    gpu->connected = false;
    gpu_wait(gpu, gpu->complete, gpu->copied);
    for (unsigned i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
        struct display_gpu_slot *slot = &gpu->slots[i];
        if (slot->commands) ID3D12GraphicsCommandList_Release(slot->commands);
        if (slot->allocator) ID3D12CommandAllocator_Release(slot->allocator);
        if (slot->texture) ID3D12Resource_Release(slot->texture);
        if (slot->source) ID3D12Resource_Release(slot->source);
        if (slot->fence) ID3D12Fence_Release(slot->fence);
        if (slot->free_fd >= 0) close(slot->free_fd);
        memset(slot, 0, sizeof(*slot));
        slot->free_fd = -1;
    }
    gpu->width = gpu->height = gpu->next = 0;
    gpu->sequence = 0;
}

void display_gpu_destroy(struct display_gpu *gpu)
{
    if (!gpu) return;
    struct drm_asb_gpu_present present = {.event_fd = -1};
    if (gpu->drm_fd >= 0) gpu_ioctl(gpu->drm_fd, DRM_IOCTL_ASB_GPU_PRESENT, &present);
    if (gpu->complete) display_gpu_disconnect(gpu);
    if (gpu->readback) {
        if (gpu->pixels) ID3D12Resource_Unmap(gpu->readback, 0, NULL);
        ID3D12Resource_Release(gpu->readback);
    }
    if (gpu->complete) ID3D12Fence_Release(gpu->complete);
    if (gpu->queue) ID3D12CommandQueue_Release(gpu->queue);
    if (gpu->device) ID3D12Device_Release(gpu->device);
    if (gpu->copy_fd >= 0) close(gpu->copy_fd);
    if (gpu->event_fd >= 0) close(gpu->event_fd);
    if (gpu->dxg_fd >= 0) close(gpu->dxg_fd);
    if (gpu->drm_fd >= 0) close(gpu->drm_fd);
    if (gpu->d3d12_lib) dlclose(gpu->d3d12_lib);
    if (gpu->dxcore_lib) dlclose(gpu->dxcore_lib);
    free(gpu);
}

struct display_gpu *display_gpu_create(int drm_fd)
{
    struct drm_asb_gpu_query query = {0};
    unsigned required = ASB_DRM_GPU_CAP_BUFFER_IMPORT | ASB_DRM_GPU_CAP_NATIVE_HANDLES |
                        ASB_DRM_GPU_CAP_SYNC_FILE;
    if (gpu_ioctl(drm_fd, DRM_IOCTL_ASB_GPU_QUERY, &query) < 0 ||
        query.version != ASB_DRM_GPU_VERSION || (query.capabilities & required) != required) {
        fprintf(stderr, "[display] GPU bridge unavailable (version=%u, capabilities=%u, errno=%d)\n",
                query.version, query.capabilities, errno);
        return NULL;
    }
    struct display_gpu *gpu = calloc(1, sizeof(*gpu));
    if (!gpu) return NULL;
    gpu->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    gpu->dxg_fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
    gpu->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    gpu->copy_fd = -1;
    for (unsigned i = 0; i < DISPLAY_GPU_SLOTS; ++i) gpu->slots[i].free_fd = -1;
    gpu->d3d12_lib = dlopen("libd3d12.so", RTLD_NOW | RTLD_LOCAL);
    gpu->dxcore_lib = dlopen("libdxcore.so", RTLD_NOW | RTLD_LOCAL);
    HRESULT (*create_factory)(REFIID, void **) = gpu->dxcore_lib
        ? dlsym(gpu->dxcore_lib, "DXCoreCreateAdapterFactory") : NULL;
    HRESULT (*create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **) = gpu->d3d12_lib
        ? dlsym(gpu->d3d12_lib, "D3D12CreateDevice") : NULL;
    IUnknown *factory = NULL, *adapters = NULL, *adapter = NULL;
    const char *stage = "runtime loading";
    HRESULT hr = E_FAIL;
    if (gpu->drm_fd < 0 || gpu->dxg_fd < 0 || gpu->event_fd < 0 ||
        !create_factory || !create_device) goto fail;
    stage = "adapter factory";
    if (FAILED(hr = create_factory(&asb_factory_iid, (void **)&factory))) goto fail;
    stage = "adapter enumeration";
    if (FAILED(hr = ((const struct dxcore_factory_vtable *)factory->lpVtbl)->CreateAdapterList(
            factory, 1, &asb_graphics_attribute, &asb_list_iid, (void **)&adapters))) goto fail;
    uint32_t count = ((const struct dxcore_list_vtable *)adapters->lpVtbl)->GetAdapterCount(adapters);
    for (uint32_t i = 0; i < count && !gpu->device; ++i) {
        if (FAILED(((const struct dxcore_list_vtable *)adapters->lpVtbl)->GetAdapter(
                adapters, i, &asb_adapter_iid, (void **)&adapter))) continue;
        stage = "D3D12 device";
        hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&gpu->device);
        IUnknown_Release(adapter);
        adapter = NULL;
    }
    if (!gpu->device) goto fail;
    D3D12_COMMAND_QUEUE_DESC queue = {0};
    stage = "copy queue";
    if (FAILED(hr = ID3D12Device_CreateCommandQueue(gpu->device, &queue, &IID_ID3D12CommandQueue,
                                               (void **)&gpu->queue))) goto fail;
    stage = "shared fence";
    if (FAILED(hr = ID3D12Device_CreateFence(gpu->device, 0, D3D12_FENCE_FLAG_SHARED,
                                       &IID_ID3D12Fence, (void **)&gpu->complete))) goto fail;
    stage = "native fence";
    if (gpu_native(gpu, (ID3D12DeviceChild *)gpu->complete, &gpu->native_complete) < 0) goto fail;
    gpu->copy_fd = gpu->native_complete.fd;
    struct drm_asb_gpu_present present = {
        .event_fd = gpu->event_fd, .dxg_fd = gpu->dxg_fd, .fence_fd = gpu->copy_fd,
    };
    stage = "presenter registration";
    if (gpu_ioctl(gpu->drm_fd, DRM_IOCTL_ASB_GPU_PRESENT, &present) < 0) goto fail;
    gpu->adapter_luid = present.adapter_luid;
    IUnknown_Release(adapters);
    IUnknown_Release(factory);
    return gpu;
fail:
    fprintf(stderr, "[display] GPU initialization failed at %s (0x%08x, errno=%d)\n",
            stage, (unsigned)hr, errno);
    if (adapter) IUnknown_Release(adapter);
    if (adapters) IUnknown_Release(adapters);
    if (factory) IUnknown_Release(factory);
    display_gpu_destroy(gpu);
    return NULL;
}

int display_gpu_event(struct display_gpu *gpu)
{
    if (!gpu) return -1;
    if (gpu->connected) {
        struct display_gpu_slot *slot = &gpu->slots[gpu->next];
        uint64_t completed = ID3D12Fence_GetCompletedValue(slot->fence);
        if (completed < slot->free_value) {
            if (slot->free_fd < 0) {
                struct drm_asb_gpu_native native;
                if (gpu_native(gpu, (ID3D12DeviceChild *)slot->fence, &native) < 0) return -1;
                struct d3dkmt_createsyncfile sync = {
                    .device.v = native.device_handle, .monitored_fence.v = native.object_handle,
                    .fence_value = slot->free_value,
                };
                int result = gpu_ioctl(gpu->dxg_fd, LX_DXCREATESYNCFILE, &sync);
                close(native.fd);
                if (result < 0) return -1;
                slot->free_fd = (int)sync.sync_file_handle;
            }
            return slot->free_fd;
        }
        if (slot->free_fd >= 0) { close(slot->free_fd); slot->free_fd = -1; }
    }
    return gpu->event_fd;
}

static int gpu_commands(struct display_gpu *gpu, struct display_gpu_slot *slot)
{
    if (gpu_wait(gpu, gpu->complete, slot->copy_value) < 0) return -1;
    if (slot->source) { ID3D12Resource_Release(slot->source); slot->source = NULL; }
    if (!slot->allocator) {
        if (FAILED(ID3D12Device_CreateCommandAllocator(gpu->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&slot->allocator))) return -1;
        if (FAILED(ID3D12Device_CreateCommandList(gpu->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&slot->commands))) return -1;
    } else {
        if (FAILED(ID3D12CommandAllocator_Reset(slot->allocator)) ||
            FAILED(ID3D12GraphicsCommandList_Reset(slot->commands, slot->allocator, NULL))) return -1;
    }
    return 0;
}

int display_gpu_healthy(struct display_gpu *gpu)
{
    return gpu && SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(gpu->device));
}

int display_gpu_active(struct display_gpu *gpu)
{
    struct drm_asb_gpu_query query = {0};
    if (!display_gpu_healthy(gpu) || gpu_ioctl(gpu->drm_fd, DRM_IOCTL_ASB_GPU_QUERY, &query) < 0)
        return 0;
    return (query.capabilities & ASB_DRM_GPU_CAP_SCANOUT) != 0;
}

int display_gpu_connect(struct display_gpu *gpu, int socket_fd,
                        const DisplayGpuPacket *query, uint32_t width, uint32_t height,
                        int resize, uint32_t size_mode)
{
    if (!gpu || !width || !height || width > DISPLAY_MAX_WIDTH || height > DISPLAY_MAX_HEIGHT ||
        query->magic != DISPLAY_GPU_MAGIC || query->version != DISPLAY_GPU_VERSION ||
        query->type != DISPLAY_GPU_QUERY || !(query->reserved & DISPLAY_GPU_CAP_FENCE)) return 0;
    display_gpu_disconnect(gpu);
    DisplayGpuPacket packet = {
        .magic = DISPLAY_GPU_MAGIC, .version = DISPLAY_GPU_VERSION, .type = DISPLAY_GPU_CAPS,
        .session = query->session, .reserved = DISPLAY_GPU_CAP_FENCE,
        .width = width, .height = height, .format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .slot = size_mode,
    };
    if (resize && (query->reserved & DISPLAY_GPU_CAP_RESIZE))
        packet.reserved |= DISPLAY_GPU_CAP_RESIZE;
    if (gpu_transfer(socket_fd, &packet, sizeof(packet), true) < 0 ||
        gpu_transfer(socket_fd, &packet, sizeof(packet), false) < 0) return -1;
    if (packet.magic != DISPLAY_GPU_MAGIC || packet.version != DISPLAY_GPU_VERSION ||
        packet.session != query->session || packet.type != DISPLAY_GPU_OFFER) return -1;
    if (packet.status) return 0;
    D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1, .VisibleNodeMask = 1};
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
        .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc.Count = 1, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
    };
    for (unsigned i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
        struct display_gpu_slot *slot = &gpu->slots[i];
        if (FAILED(ID3D12Device_CreateCommittedResource(gpu->device, &heap, D3D12_HEAP_FLAG_SHARED,
                &desc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&slot->texture)) ||
            FAILED(ID3D12Device_CreateFence(gpu->device, 0, D3D12_FENCE_FLAG_SHARED,
                &IID_ID3D12Fence, (void **)&slot->fence))) goto fail;
    }
    for (unsigned i = 0; i < DISPLAY_GPU_FENCE_SLOTS; ++i) {
        ID3D12DeviceChild *object = i < DISPLAY_GPU_SLOTS
            ? (ID3D12DeviceChild *)gpu->slots[i].texture
            : (ID3D12DeviceChild *)gpu->slots[i - DISPLAY_GPU_SLOTS].fence;
        packet = (DisplayGpuPacket){
            .magic = DISPLAY_GPU_MAGIC, .version = DISPLAY_GPU_VERSION, .type = DISPLAY_GPU_OFFER,
            .session = query->session, .slot = i, .width = width, .height = height,
            .format = DXGI_FORMAT_B8G8R8A8_UNORM,
        };
        if (gpu_export(gpu, object, &packet.handle) < 0 ||
            gpu_transfer(socket_fd, &packet, sizeof(packet), true) < 0 ||
            gpu_transfer(socket_fd, &packet, sizeof(packet), false) < 0) goto fail;
        if (packet.magic != DISPLAY_GPU_MAGIC || packet.version != DISPLAY_GPU_VERSION ||
            packet.session != query->session || packet.type != DISPLAY_GPU_READY ||
            packet.slot != i || packet.status) goto fail;
    }
    gpu->width = width;
    gpu->height = height;
    gpu->connected = true;
    return 1;
fail:
    display_gpu_disconnect(gpu);
    return -1;
}

static int gpu_acquire(struct display_gpu *gpu, struct drm_asb_gpu_acquire *frame)
{
    uint64_t notification;
    while (read(gpu->event_fd, &notification, sizeof(notification)) > 0) {}
    if (FAILED(ID3D12Device_GetDeviceRemovedReason(gpu->device))) return -1;
    struct d3dkmt_createsyncfile sync = {
        .device.v = gpu->native_complete.device_handle,
        .monitored_fence.v = gpu->native_complete.object_handle,
        .fence_value = ++gpu->copied,
    };
    if (gpu_ioctl(gpu->dxg_fd, LX_DXCREATESYNCFILE, &sync) < 0) return -1;
    *frame = (struct drm_asb_gpu_acquire){
        .complete_fd = (int)sync.sync_file_handle, .fd = -1, .sequence = gpu->sequence,
    };
    int result = gpu_ioctl(gpu->drm_fd, DRM_IOCTL_ASB_GPU_ACQUIRE, frame);
    int error = errno;
    close(frame->complete_fd);
    if (result < 0) {
        if (FAILED(ID3D12CommandQueue_Signal(gpu->queue, gpu->complete, gpu->copied))) return -1;
        return error == EAGAIN ? 0 : error == ENODATA ? -2 : -1;
    }
    return 1;
}

static int gpu_copy(struct display_gpu *gpu, struct display_gpu_slot *slot,
                    const struct drm_asb_gpu_acquire *frame, bool readback)
{
    if (gpu_commands(gpu, slot) < 0) return -1;
    if (frame->adapter_luid != gpu->adapter_luid ||
        FAILED(ID3D12Device_OpenSharedHandle(gpu->device, (HANDLE)(uintptr_t)frame->fd,
            &IID_ID3D12Resource, (void **)&slot->source))) return -1;
    if (readback) {
        D3D12_TEXTURE_COPY_LOCATION source = {.pResource = slot->source,
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION target = {.pResource = gpu->readback,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = gpu->footprint};
        ID3D12GraphicsCommandList_CopyTextureRegion(slot->commands, &target, 0, 0, 0, &source, NULL);
    } else {
        if (FAILED(ID3D12CommandQueue_Wait(gpu->queue, slot->fence, slot->free_value))) return -1;
        ID3D12GraphicsCommandList_CopyResource(slot->commands, slot->texture, slot->source);
    }
    if (FAILED(ID3D12GraphicsCommandList_Close(slot->commands))) return -1;
    ID3D12CommandList *commands[] = {(ID3D12CommandList *)slot->commands};
    ID3D12CommandQueue_ExecuteCommandLists(gpu->queue, 1, commands);
    slot->copy_value = gpu->copied;
    if (FAILED(ID3D12CommandQueue_Signal(gpu->queue, gpu->complete, gpu->copied))) return -1;
    if (!readback) {
        if (FAILED(ID3D12CommandQueue_Signal(gpu->queue, slot->fence, slot->free_value + 1))) return -1;
        slot->free_value += 2;
    }
    gpu->sequence = frame->sequence;
    return 0;
}

int display_gpu_frame(struct display_gpu *gpu)
{
    if (!gpu || !gpu->connected) return -1;
    struct display_gpu_slot *slot = &gpu->slots[gpu->next];
    uint64_t completed = ID3D12Fence_GetCompletedValue(slot->fence);
    if (completed == UINT64_MAX) return -1;
    if (completed < slot->free_value) return 0;
    struct drm_asb_gpu_acquire frame;
    int result = gpu_acquire(gpu, &frame);
    if (result <= 0) return result;
    if (frame.width != gpu->width || frame.height != gpu->height) result = -2;
    else result = gpu_copy(gpu, slot, &frame, false) < 0 ? -1 : 1;
    close(frame.fd);
    if (result < 0) ID3D12CommandQueue_Signal(gpu->queue, gpu->complete, gpu->copied);
    if (result > 0) gpu->next = (gpu->next + 1) % DISPLAY_GPU_SLOTS;
    return result;
}

int display_gpu_readback(struct display_gpu *gpu, const void **pixels,
                         uint32_t *width, uint32_t *height, uint32_t *stride)
{
    if (!gpu || gpu->connected) return 0;
    struct drm_asb_gpu_acquire frame;
    int result = gpu_acquire(gpu, &frame);
    if (result == -2) return 0;
    if (result < 0) return -1;
    if (!result) goto done;
    if (!gpu->readback || gpu->readback_width != frame.width || gpu->readback_height != frame.height) {
        if (gpu->readback) {
            ID3D12Resource_Unmap(gpu->readback, 0, NULL);
            ID3D12Resource_Release(gpu->readback);
            gpu->readback = NULL;
            gpu->pixels = NULL;
        }
        D3D12_RESOURCE_DESC image = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Width = frame.width, .Height = frame.height, .DepthOrArraySize = 1, .MipLevels = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM, .SampleDesc.Count = 1};
        UINT64 size;
        ID3D12Device_GetCopyableFootprints(gpu->device, &image, 0, 1, 0, &gpu->footprint, NULL, NULL, &size);
        D3D12_RESOURCE_DESC buffer = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
            .SampleDesc.Count = 1, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK};
        if (FAILED(ID3D12Device_CreateCommittedResource(gpu->device, &heap, D3D12_HEAP_FLAG_NONE,
                &buffer, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void **)&gpu->readback)) ||
            FAILED(ID3D12Resource_Map(gpu->readback, 0, NULL, &gpu->pixels))) goto fail;
        gpu->readback_width = frame.width;
        gpu->readback_height = frame.height;
    }
    if (gpu_copy(gpu, &gpu->slots[0], &frame, true) < 0 ||
        gpu_wait(gpu, gpu->complete, gpu->copied) < 0) goto fail;
    close(frame.fd);
done:
    *pixels = gpu->pixels;
    *width = gpu->readback_width;
    *height = gpu->readback_height;
    *stride = gpu->footprint.Footprint.RowPitch;
    return gpu->pixels ? 1 : 0;
fail:
    close(frame.fd);
    ID3D12CommandQueue_Signal(gpu->queue, gpu->complete, gpu->copied);
    return -1;
}
