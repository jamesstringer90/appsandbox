#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_prime.h>

#include "asb_drm.h"
#include "asb_drm_uapi.h"

struct asb_gpu_object {
	struct drm_gem_object base;
	struct list_head entry;
	struct file *resource;
	void *identity;
	u64 adapter_luid;
	u32 width;
	u32 height;
	u32 format;
	u32 pitch;
};

extern int dxgk_shared_resource_info(struct file *file, u64 *adapter_luid,
				     void **identity);
extern int dxgk_shared_object_info(struct file *dxg_file, struct file *shared_file,
				 u32 *device_handle, u32 *object_handle,
				 u64 *adapter_luid);

static int asb_gpu_resource_info(struct file *file, u64 *luid, void **identity)
{
	int (*query)(struct file *, u64 *, void **);
	int ret;

	query = symbol_get(dxgk_shared_resource_info);
	if (!query)
		return -EOPNOTSUPP;
	ret = query(file, luid, identity);
	symbol_put(dxgk_shared_resource_info);
	return ret;
}

static void asb_gpu_free(struct drm_gem_object *obj)
{
	struct asb_gpu_object *gpu = container_of(obj, struct asb_gpu_object, base);
	struct asb_device *asb = to_asb(obj->dev);

	mutex_lock(&asb->gpu_lock);
	list_del(&gpu->entry);
	mutex_unlock(&asb->gpu_lock);
	fput(gpu->resource);
	drm_gem_object_release(obj);
	kfree(gpu);
}

static int asb_gpu_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	return -EOPNOTSUPP;
}

static const struct drm_gem_object_funcs asb_gpu_object_funcs = {
	.free = asb_gpu_free,
	.mmap = asb_gpu_mmap,
};

int asb_gpu_query_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asb_gpu_query *args = data;
	struct asb_device *asb = to_asb(drm);
	int (*query)(struct file *, u64 *, void **);
	int (*native)(struct file *, struct file *, u32 *, u32 *, u64 *);

	args->version = ASB_DRM_GPU_VERSION;
	args->capabilities = 0;
	args->max_width = ASB_MAX_WIDTH;
	args->max_height = ASB_MAX_HEIGHT;
	args->adapter_luid = 0;
	query = symbol_get(dxgk_shared_resource_info);
	if (query) {
		args->capabilities = ASB_DRM_GPU_CAP_BUFFER_IMPORT;
		symbol_put(dxgk_shared_resource_info);
	}
	native = symbol_get(dxgk_shared_object_info);
	if (native) {
		args->capabilities |= ASB_DRM_GPU_CAP_NATIVE_HANDLES |
				      ASB_DRM_GPU_CAP_SYNC_FILE;
		symbol_put(dxgk_shared_object_info);
	}
	mutex_lock(&asb->gpu_lock);
	if (asb->gpu_presenter) {
		args->capabilities |= ASB_DRM_GPU_CAP_PRESENT;
		args->adapter_luid = asb->gpu_adapter_luid;
		if (asb->gpu_scanout)
			args->capabilities |= ASB_DRM_GPU_CAP_SCANOUT;
	}
	mutex_unlock(&asb->gpu_lock);
	return 0;
}

void asb_gpu_update(struct asb_device *asb, struct drm_framebuffer *fb)
{
	struct drm_framebuffer *old;

	if (fb && drm_gem_fb_get_obj(fb, 0)->funcs != &asb_gpu_object_funcs)
		fb = NULL;
	if (fb)
		drm_framebuffer_get(fb);
	mutex_lock(&asb->gpu_lock);
	old = asb->gpu_scanout;
	asb->gpu_scanout = fb;
	++asb->gpu_sequence;
	if (asb->gpu_event)
		eventfd_signal(asb->gpu_event);
	mutex_unlock(&asb->gpu_lock);
	if (old)
		drm_framebuffer_put(old);
}

void asb_gpu_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct asb_device *asb = to_asb(drm);
	struct eventfd_ctx *event = NULL;

	mutex_lock(&asb->gpu_lock);
	if (asb->gpu_presenter == file) {
		event = asb->gpu_event;
		asb->gpu_event = NULL;
		asb->gpu_presenter = NULL;
		asb->gpu_adapter_luid = 0;
	}
	mutex_unlock(&asb->gpu_lock);
	if (event)
		eventfd_ctx_put(event);
}

void asb_gpu_fini(struct asb_device *asb)
{
	struct eventfd_ctx *event;

	mutex_lock(&asb->gpu_lock);
	event = asb->gpu_event;
	asb->gpu_event = NULL;
	asb->gpu_presenter = NULL;
	asb->gpu_adapter_luid = 0;
	mutex_unlock(&asb->gpu_lock);
	if (event) {
		eventfd_signal(event);
		eventfd_ctx_put(event);
	}
	asb_gpu_update(asb, NULL);
}

int asb_gpu_present_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asb_gpu_present *args = data;
	struct asb_device *asb = to_asb(drm);
	struct drm_asb_gpu_native native = {
		.dxg_fd = args->dxg_fd,
		.fd = args->fence_fd,
	};
	struct eventfd_ctx *event, *old = NULL;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (args->flags || args->adapter_luid || args->event_fd < -1)
		return -EINVAL;
	if (args->event_fd == -1) {
		asb_gpu_postclose(drm, file);
		return 0;
	}
	ret = asb_gpu_native_ioctl(drm, &native, file);
	if (ret)
		return ret;
	event = eventfd_ctx_fdget(args->event_fd);
	if (IS_ERR(event))
		return PTR_ERR(event);
	mutex_lock(&asb->gpu_lock);
	if (asb->gpu_presenter &&
	    (asb->gpu_presenter != file ||
	     asb->gpu_adapter_luid != native.adapter_luid)) {
		ret = -EBUSY;
	} else {
		old = asb->gpu_event;
		asb->gpu_event = event;
		asb->gpu_presenter = file;
		asb->gpu_adapter_luid = native.adapter_luid;
		args->adapter_luid = native.adapter_luid;
		eventfd_signal(event);
	}
	mutex_unlock(&asb->gpu_lock);
	if (ret)
		eventfd_ctx_put(event);
	if (old)
		eventfd_ctx_put(old);
	return ret;
}

int asb_gpu_acquire_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asb_gpu_acquire *args = data;
	struct asb_device *asb = to_asb(drm);
	struct asb_gpu_object *gpu;
	struct dma_fence *complete;
	struct drm_gem_object *obj;
	int fd = -1, ret;

	if (args->flags)
		return -EINVAL;
	complete = sync_file_get_fence(args->complete_fd);
	if (!complete)
		return -EINVAL;
	ret = dma_fence_get_status(complete);
	if (ret) {
		if (ret > 0)
			ret = -EINVAL;
		goto out_fence;
	}
	mutex_lock(&asb->gpu_lock);
	if (asb->gpu_presenter != file) {
		ret = -EACCES;
		goto out_unlock;
	}
	if (!asb->gpu_scanout) {
		ret = -ENODATA;
		goto out_unlock;
	}
	if (args->sequence == asb->gpu_sequence) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	obj = drm_gem_fb_get_obj(asb->gpu_scanout, 0);
	gpu = container_of(obj, struct asb_gpu_object, base);
	if (gpu->adapter_luid != asb->gpu_adapter_luid) {
		ret = -EXDEV;
		goto out_unlock;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_unlock;
	}
	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		goto out_unlock;
	ret = dma_resv_reserve_fences(obj->resv, 1);
	if (!ret) {
		dma_resv_add_fence(obj->resv, complete, DMA_RESV_USAGE_READ);
		args->fd = fd;
		args->width = gpu->width;
		args->height = gpu->height;
		args->format = gpu->format;
		args->pitch = gpu->pitch;
		args->sequence = asb->gpu_sequence;
		args->adapter_luid = gpu->adapter_luid;
		fd_install(fd, get_file(gpu->resource));
		fd = -1;
	}
	dma_resv_unlock(obj->resv);
out_unlock:
	mutex_unlock(&asb->gpu_lock);
	if (fd >= 0)
		put_unused_fd(fd);
out_fence:
	dma_fence_put(complete);
	return ret;
}

int asb_gpu_native_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_asb_gpu_native *args = data;
	int (*query)(struct file *, struct file *, u32 *, u32 *, u64 *);
	struct file *dxg_file, *shared_file;
	int ret;

	if (args->flags || args->device_handle || args->object_handle ||
	    args->adapter_luid)
		return -EINVAL;
	dxg_file = fget(args->dxg_fd);
	if (!dxg_file)
		return -EBADF;
	shared_file = fget(args->fd);
	if (!shared_file) {
		ret = -EBADF;
		goto out_dxg;
	}
	query = symbol_get(dxgk_shared_object_info);
	if (query) {
		ret = query(dxg_file, shared_file, &args->device_handle,
			    &args->object_handle, &args->adapter_luid);
		symbol_put(dxgk_shared_object_info);
	} else {
		ret = -EOPNOTSUPP;
	}
	fput(shared_file);
out_dxg:
	fput(dxg_file);
	return ret;
}

int asb_gpu_import_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct drm_asb_gpu_import *args = data;
	struct asb_device *asb = to_asb(drm);
	struct asb_gpu_object *gpu;
	struct file *resource;
	void *identity;
	u64 luid;
	int ret;

	if (args->flags || args->handle || args->adapter_luid ||
	    !args->width || args->width > ASB_MAX_WIDTH ||
	    !args->height || args->height > ASB_MAX_HEIGHT ||
	    (args->format != DRM_FORMAT_XRGB8888 &&
	     args->format != DRM_FORMAT_ARGB8888) ||
	    args->pitch < args->width * 4 ||
	    args->pitch > ALIGN(ASB_MAX_WIDTH * 4, 256) ||
	    (args->pitch & 3))
		return -EINVAL;

	resource = fget(args->fd);
	if (!resource)
		return -EBADF;
	ret = asb_gpu_resource_info(resource, &luid, &identity);
	if (ret)
		goto out_file;

	mutex_lock(&asb->gpu_lock);
	list_for_each_entry(gpu, &asb->gpu_objects, entry) {
		if (gpu->identity != identity)
			continue;
		if (gpu->width != args->width || gpu->height != args->height ||
		    gpu->format != args->format || gpu->pitch != args->pitch) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (kref_get_unless_zero(&gpu->base.refcount))
			goto found;
	}

	gpu = kzalloc(sizeof(*gpu), GFP_KERNEL);
	if (!gpu) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	drm_gem_private_object_init(drm, &gpu->base,
				   PAGE_ALIGN((u64)args->pitch * args->height));
	gpu->base.funcs = &asb_gpu_object_funcs;
	gpu->resource = get_file(resource);
	gpu->identity = identity;
	gpu->adapter_luid = luid;
	gpu->width = args->width;
	gpu->height = args->height;
	gpu->format = args->format;
	gpu->pitch = args->pitch;
	list_add(&gpu->entry, &asb->gpu_objects);

found:
	mutex_unlock(&asb->gpu_lock);
	ret = drm_gem_handle_create(file, &gpu->base, &args->handle);
	if (!ret)
		args->adapter_luid = gpu->adapter_luid;
	drm_gem_object_put(&gpu->base);
	goto out_file;

out_unlock:
	mutex_unlock(&asb->gpu_lock);
out_file:
	fput(resource);
	return ret;
}

int asb_gpu_export_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct drm_asb_gpu_export *args = data;
	struct drm_gem_object *obj;
	struct asb_gpu_object *gpu;
	struct dma_buf *dma_buf;
	int fd;

	if ((args->flags & ~ASB_DRM_GPU_EXPORT_PRIME_FD) || args->reserved)
		return -EINVAL;
	if (args->flags & ASB_DRM_GPU_EXPORT_PRIME_FD) {
		if (args->handle)
			return -EINVAL;
		dma_buf = dma_buf_get(args->fd);
		if (IS_ERR(dma_buf))
			return PTR_ERR(dma_buf);
		obj = drm_gem_prime_import(drm, dma_buf);
		dma_buf_put(dma_buf);
		if (IS_ERR(obj))
			return PTR_ERR(obj);
	} else {
		obj = drm_gem_object_lookup(file, args->handle);
		if (!obj)
			return -ENOENT;
	}
	if (obj->funcs != &asb_gpu_object_funcs) {
		fd = -EINVAL;
		goto out;
	}
	gpu = container_of(obj, struct asb_gpu_object, base);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		goto out;
	args->fd = fd;
	args->width = gpu->width;
	args->height = gpu->height;
	args->format = gpu->format;
	args->pitch = gpu->pitch;
	args->adapter_luid = gpu->adapter_luid;
	fd_install(fd, get_file(gpu->resource));
	fd = 0;
out:
	drm_gem_object_put(obj);
	return fd;
}

struct drm_framebuffer *asb_fb_create(struct drm_device *drm,
				    struct drm_file *file,
				    const struct drm_format_info *info,
				    const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct asb_gpu_object *gpu;

	fb = drm_gem_fb_create(drm, file, info, cmd);
	if (IS_ERR(fb))
		return fb;
	obj = drm_gem_fb_get_obj(fb, 0);
	if (obj->funcs != &asb_gpu_object_funcs)
		return fb;
	gpu = container_of(obj, struct asb_gpu_object, base);
	if (!cmd->flags && !cmd->offsets[0] &&
	    cmd->width == gpu->width && cmd->height == gpu->height &&
	    cmd->pixel_format == gpu->format && cmd->pitches[0] == gpu->pitch)
		return fb;
	drm_framebuffer_put(fb);
	return ERR_PTR(-EINVAL);
}
