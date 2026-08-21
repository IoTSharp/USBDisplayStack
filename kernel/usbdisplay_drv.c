// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual DRM/fbdev producer for userspace USB display protocol backends.
 *
 * Applications render through normal DRM or fbdev interfaces. A userspace
 * daemon consumes coherent snapshots from /dev/usbdisplay0 and owns all
 * compression, protocol and USB transport details.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/tinydrm/tinydrm.h>

#include <usbdisplay/uapi.h>

#define USBDISPLAY_DRIVER_NAME "usbdisplay"
#define USBDISPLAY_DRIVER_DESC "Virtual USB display frontend"
#define USBDISPLAY_DRIVER_DATE "20260807"
#define USBDISPLAY_MAX_WIDTH 4096U
#define USBDISPLAY_MAX_HEIGHT 2160U
#define USBDISPLAY_FB_BPP 32U
#define USBDISPLAY_PALETTE_SIZE 16U

static unsigned int width = 1920;
module_param(width, uint, 0444);
MODULE_PARM_DESC(width, "Virtual display width");

static unsigned int height = 1080;
module_param(height, uint, 0444);
MODULE_PARM_DESC(height, "Virtual display height");

struct usbdisplay_device {
	struct tinydrm_device tinydrm;
	struct drm_display_mode mode;
	struct fb_info *fb_info;
	struct fb_deferred_io fb_deferred;
	struct miscdevice miscdev;
	struct mutex publish_lock;
	wait_queue_head_t update_wait;
	void *frame_ring;
	void *fb_memory;
	size_t slot_bytes;
	size_t map_bytes;
	size_t fb_bytes;
	u64 sequence;
	unsigned int next_slot;
	int held_slot;
	atomic_t consumer_open;
	atomic_t producer_open;
	u32 pseudo_palette[USBDISPLAY_PALETTE_SIZE];
	struct usbdisplay_update latest_update;
};

struct usbdisplay_file {
	struct usbdisplay_device *udev;
	u64 last_sequence;
	int held_slot;
};

static struct platform_device *usbdisplay_platform_device;

static int usbdisplay_publish_initial(struct usbdisplay_device *udev);

static uint32_t usbdisplay_format_from_drm(uint32_t drm_format)
{
	uint32_t format = USBDISPLAY_FORMAT_INVALID;

	if (drm_format == DRM_FORMAT_XRGB8888) {
		format = USBDISPLAY_FORMAT_XRGB8888;
	} else if (drm_format == DRM_FORMAT_RGB565) {
		format = USBDISPLAY_FORMAT_RGB565;
	}

	return format;
}

static unsigned int usbdisplay_bytes_per_pixel(uint32_t format)
{
	unsigned int bytes = 0;

	if (format == USBDISPLAY_FORMAT_XRGB8888) {
		bytes = 4;
	} else if (format == USBDISPLAY_FORMAT_RGB565) {
		bytes = 2;
	}

	return bytes;
}

static void usbdisplay_full_damage(struct usbdisplay_update *update)
{
	update->damage_x = 0;
	update->damage_y = 0;
	update->damage_width = update->width;
	update->damage_height = update->height;
}

static void usbdisplay_set_damage(struct usbdisplay_update *update,
				  struct drm_clip_rect *clips,
				  unsigned int num_clips)
{
	unsigned int index;
	unsigned int x1;
	unsigned int y1;
	unsigned int x2;
	unsigned int y2;

	usbdisplay_full_damage(update);
	if (clips != NULL && num_clips > 0) {
		x1 = update->width;
		y1 = update->height;
		x2 = 0;
		y2 = 0;
		for (index = 0; index < num_clips; ++index) {
			x1 = min_t(unsigned int, x1, clips[index].x1);
			y1 = min_t(unsigned int, y1, clips[index].y1);
			x2 = max_t(unsigned int, x2, clips[index].x2);
			y2 = max_t(unsigned int, y2, clips[index].y2);
		}
		x1 = min(x1, update->width);
		y1 = min(y1, update->height);
		x2 = min(x2, update->width);
		y2 = min(y2, update->height);
		if (x2 > x1 && y2 > y1) {
			update->damage_x = x1;
			update->damage_y = y1;
			update->damage_width = x2 - x1;
			update->damage_height = y2 - y1;
		}
	}
}

static int usbdisplay_select_slot(struct usbdisplay_device *udev)
{
	unsigned int attempt;
	unsigned int slot;
	int selected = -1;

	for (attempt = 0; attempt < USBDISPLAY_SLOT_COUNT; ++attempt) {
		slot = (udev->next_slot + attempt) % USBDISPLAY_SLOT_COUNT;
		if ((int)slot != udev->held_slot) {
			selected = (int)slot;
			udev->next_slot = (slot + 1) % USBDISPLAY_SLOT_COUNT;
			break;
		}
	}

	return selected;
}

static int usbdisplay_publish(struct usbdisplay_device *udev,
			      const void *pixels,
			      unsigned int frame_width,
			      unsigned int frame_height,
			      unsigned int stride,
			      uint32_t format,
			      struct drm_clip_rect *clips,
			      unsigned int num_clips,
			      uint32_t source)
{
	struct usbdisplay_update update;
	size_t frame_bytes;
	void *destination;
	int slot;
	int result = 0;

	memset(&update, 0, sizeof(update));
	if (pixels == NULL || usbdisplay_bytes_per_pixel(format) == 0) {
		result = -EINVAL;
	} else if (frame_width > width || frame_height > height) {
		result = -EINVAL;
	} else if (frame_height != 0 &&
		   (size_t)stride > udev->slot_bytes / frame_height) {
		result = -EOVERFLOW;
	} else {
		frame_bytes = (size_t)stride * frame_height;
		if (frame_bytes > udev->slot_bytes) {
			result = -EOVERFLOW;
		} else {
			mutex_lock(&udev->publish_lock);
			slot = usbdisplay_select_slot(udev);
			if (slot < 0) {
				result = -EBUSY;
			} else {
				destination = (char *)udev->frame_ring +
					      ((size_t)slot * udev->slot_bytes);
				memcpy(destination, pixels, frame_bytes);
				update.sequence = ++udev->sequence;
				update.timestamp_ns = ktime_get_ns();
				update.slot = (uint32_t)slot;
				update.width = frame_width;
				update.height = frame_height;
				update.stride = stride;
				update.format = format;
				update.source = source;
				usbdisplay_set_damage(&update, clips, num_clips);
				smp_wmb();
				udev->latest_update = update;
			}
			mutex_unlock(&udev->publish_lock);
			if (result == 0) {
				wake_up_interruptible(&udev->update_wait);
			}
		}
	}

	return result;
}

static int usbdisplay_fbdev_publish(struct fb_info *info)
{
	struct usbdisplay_device *udev = info->par;
	int result;

	result = usbdisplay_publish(udev, info->screen_buffer,
				    info->var.xres, info->var.yres,
				    info->fix.line_length,
				    USBDISPLAY_FORMAT_XRGB8888, NULL, 0,
				    USBDISPLAY_SOURCE_FBDEV);

	return result;
}

/* 仅统计真实用户态生产者；最后一个应用退出后通知守护进程恢复状态页。 */
static void usbdisplay_producer_opened(struct usbdisplay_device *udev)
{
	atomic_inc(&udev->producer_open);
}

static void usbdisplay_producer_closed(struct usbdisplay_device *udev)
{
	if (atomic_dec_and_test(&udev->producer_open)) {
		usbdisplay_publish_initial(udev);
	}
}

static int usbdisplay_fbdev_open(struct fb_info *info, int user)
{
	int result = 0;

	if (user != 0) {
		usbdisplay_producer_opened(info->par);
	}

	return result;
}

static int usbdisplay_fbdev_release(struct fb_info *info, int user)
{
	int result = 0;

	if (user != 0) {
		usbdisplay_producer_closed(info->par);
	}

	return result;
}

static ssize_t usbdisplay_fbdev_write(struct fb_info *info,
				      const char __user *buffer,
				      size_t count, loff_t *position)
{
	ssize_t result;

	result = fb_sys_write(info, buffer, count, position);
	if (result > 0) {
		usbdisplay_fbdev_publish(info);
	}

	return result;
}

static void usbdisplay_fbdev_fillrect(struct fb_info *info,
				      const struct fb_fillrect *rectangle)
{
	sys_fillrect(info, rectangle);
	usbdisplay_fbdev_publish(info);
}

static void usbdisplay_fbdev_copyarea(struct fb_info *info,
				      const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	usbdisplay_fbdev_publish(info);
}

static void usbdisplay_fbdev_imageblit(struct fb_info *info,
				       const struct fb_image *image)
{
	sys_imageblit(info, image);
	usbdisplay_fbdev_publish(info);
}

static int usbdisplay_fbdev_check_var(struct fb_var_screeninfo *variable,
				      struct fb_info *info)
{
	int result = 0;

	if (variable->xres != width || variable->yres != height ||
	    variable->bits_per_pixel != USBDISPLAY_FB_BPP ||
	    variable->xres_virtual != width ||
	    variable->yres_virtual != height) {
		result = -EINVAL;
	} else {
		variable->xoffset = 0;
		variable->yoffset = 0;
		variable->red.offset = 16;
		variable->red.length = 8;
		variable->green.offset = 8;
		variable->green.length = 8;
		variable->blue.offset = 0;
		variable->blue.length = 8;
		variable->transp.offset = 24;
		variable->transp.length = 0;
		variable->red.msb_right = 0;
		variable->green.msb_right = 0;
		variable->blue.msb_right = 0;
		variable->transp.msb_right = 0;
		variable->grayscale = 0;
		variable->nonstd = 0;
		variable->rotate = FB_ROTATE_UR;
	}
	(void)info;

	return result;
}

static int usbdisplay_fbdev_setcolreg(unsigned int register_number,
				     unsigned int red,
				     unsigned int green,
				     unsigned int blue,
				     unsigned int transparency,
				     struct fb_info *info)
{
	u32 value;
	int result = 0;

	if (register_number >= USBDISPLAY_PALETTE_SIZE) {
		result = -EINVAL;
	} else {
		red >>= 8;
		green >>= 8;
		blue >>= 8;
		transparency >>= 8;
		value = (red << info->var.red.offset) |
			(green << info->var.green.offset) |
			(blue << info->var.blue.offset);
		if (info->var.transp.length != 0) {
			value |= transparency << info->var.transp.offset;
		}
		((u32 *)info->pseudo_palette)[register_number] = value;
	}

	return result;
}

static int usbdisplay_fbdev_sync(struct fb_info *info)
{
	return usbdisplay_fbdev_publish(info);
}

static int usbdisplay_fbdev_mmap(struct fb_info *info,
				 struct vm_area_struct *vma)
{
	size_t requested = vma->vm_end - vma->vm_start;
	int result = 0;

	if (vma->vm_pgoff != 0 || requested > info->fix.smem_len) {
		result = -EINVAL;
	} else {
		result = fb_deferred_io_mmap(info, vma);
	}

	return result;
}

static void usbdisplay_fbdev_deferred_io(struct fb_info *info,
					 struct list_head *page_list)
{
	(void)page_list;
	usbdisplay_fbdev_publish(info);
}

static struct fb_ops usbdisplay_fbdev_ops = {
	.owner = THIS_MODULE,
	.fb_open = usbdisplay_fbdev_open,
	.fb_release = usbdisplay_fbdev_release,
	.fb_read = fb_sys_read,
	.fb_write = usbdisplay_fbdev_write,
	.fb_check_var = usbdisplay_fbdev_check_var,
	.fb_setcolreg = usbdisplay_fbdev_setcolreg,
	.fb_fillrect = usbdisplay_fbdev_fillrect,
	.fb_copyarea = usbdisplay_fbdev_copyarea,
	.fb_imageblit = usbdisplay_fbdev_imageblit,
	.fb_sync = usbdisplay_fbdev_sync,
	.fb_mmap = usbdisplay_fbdev_mmap,
};

static int usbdisplay_fb_dirty(struct drm_framebuffer *fb,
			       struct drm_file *file_priv,
			       unsigned int flags,
			       unsigned int color,
			       struct drm_clip_rect *clips,
			       unsigned int num_clips)
{
	struct usbdisplay_device *udev = fb->dev->dev_private;
	struct drm_gem_cma_object *cma_object;
	uint32_t format;
	int result = 0;

	(void)file_priv;
	(void)flags;
	(void)color;
	cma_object = drm_fb_cma_get_gem_obj(fb, 0);
	format = usbdisplay_format_from_drm(fb->format->format);
	if (cma_object == NULL || cma_object->vaddr == NULL) {
		result = -ENODEV;
	} else {
		result = usbdisplay_publish(udev, cma_object->vaddr, fb->width,
					    fb->height, fb->pitches[0], format,
					    clips, num_clips,
					    USBDISPLAY_SOURCE_DRM);
	}

	return result;
}

static const struct drm_framebuffer_funcs usbdisplay_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = usbdisplay_fb_dirty,
};

static void usbdisplay_pipe_update(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_state)
{
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	bool helper_flushes = fb != NULL && fb != old_state->fb;

	tinydrm_display_pipe_update(pipe, old_state);
	if (fb != NULL && !helper_flushes && fb->funcs->dirty != NULL) {
		fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);
	}
}

static const struct drm_simple_display_pipe_funcs usbdisplay_pipe_funcs = {
	.update = usbdisplay_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

DEFINE_DRM_GEM_CMA_FOPS(usbdisplay_drm_fops);

/* DRM 文件生命周期与 fbdev 共用生产者计数，避免静态业务画面被误判为空闲。 */
static int usbdisplay_drm_open(struct drm_device *device,
			       struct drm_file *file)
{
	int result = 0;

	(void)file;
	usbdisplay_producer_opened(device->dev_private);

	return result;
}

static void usbdisplay_drm_postclose(struct drm_device *device,
				     struct drm_file *file)
{
	(void)file;
	usbdisplay_producer_closed(device->dev_private);
}

static struct drm_driver usbdisplay_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
			   DRIVER_ATOMIC,
	.fops = &usbdisplay_drm_fops,
	.open = usbdisplay_drm_open,
	.postclose = usbdisplay_drm_postclose,
	TINYDRM_GEM_DRIVER_OPS,
	.name = USBDISPLAY_DRIVER_NAME,
	.desc = USBDISPLAY_DRIVER_DESC,
	.date = USBDISPLAY_DRIVER_DATE,
	.major = 0,
	.minor = 1,
};

static int usbdisplay_stream_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct usbdisplay_device *udev;
	struct usbdisplay_file *context;
	int result = 0;

	(void)inode;
	udev = container_of(miscdev, struct usbdisplay_device, miscdev);
	if (atomic_cmpxchg(&udev->consumer_open, 0, 1) != 0) {
		result = -EBUSY;
	} else {
		context = kzalloc(sizeof(*context), GFP_KERNEL);
		if (context == NULL) {
			atomic_set(&udev->consumer_open, 0);
			result = -ENOMEM;
		} else {
			context->udev = udev;
			context->held_slot = -1;
			mutex_lock(&udev->publish_lock);
			context->last_sequence = udev->sequence > 0 ?
						 udev->sequence - 1 : 0;
			mutex_unlock(&udev->publish_lock);
			file->private_data = context;
		}
	}

	return result;
}

static int usbdisplay_stream_release(struct inode *inode, struct file *file)
{
	struct usbdisplay_file *context = file->private_data;
	struct usbdisplay_device *udev;

	(void)inode;
	if (context != NULL) {
		udev = context->udev;
		mutex_lock(&udev->publish_lock);
		if (udev->held_slot == context->held_slot) {
			udev->held_slot = -1;
		}
		mutex_unlock(&udev->publish_lock);
		atomic_set(&udev->consumer_open, 0);
		kfree(context);
	}

	return 0;
}

static ssize_t usbdisplay_stream_read(struct file *file, char __user *buffer,
				      size_t count, loff_t *position)
{
	struct usbdisplay_file *context = file->private_data;
	struct usbdisplay_device *udev = context->udev;
	struct usbdisplay_update update;
	ssize_t result = 0;
	int wait_result;

	(void)position;
	if (count < sizeof(update)) {
		result = -EINVAL;
	} else {
		if ((file->f_flags & O_NONBLOCK) != 0 &&
		    READ_ONCE(udev->sequence) == context->last_sequence) {
			result = -EAGAIN;
		} else {
			wait_result = wait_event_interruptible(udev->update_wait,
				READ_ONCE(udev->sequence) != context->last_sequence);
			if (wait_result != 0) {
				result = wait_result;
			} else {
				mutex_lock(&udev->publish_lock);
				update = udev->latest_update;
				udev->held_slot = (int)update.slot;
				context->held_slot = (int)update.slot;
				context->last_sequence = update.sequence;
				mutex_unlock(&udev->publish_lock);
				if (copy_to_user(buffer, &update, sizeof(update)) != 0) {
					result = -EFAULT;
				} else {
					result = sizeof(update);
				}
			}
		}
	}

	return result;
}

static unsigned int usbdisplay_stream_poll(struct file *file,
					   poll_table *wait)
{
	struct usbdisplay_file *context = file->private_data;
	struct usbdisplay_device *udev = context->udev;
	unsigned int mask = 0;

	poll_wait(file, &udev->update_wait, wait);
	if (READ_ONCE(udev->sequence) != context->last_sequence) {
		mask = POLLIN | POLLRDNORM;
	}

	return mask;
}

static long usbdisplay_stream_ioctl(struct file *file, unsigned int command,
				    unsigned long argument)
{
	struct usbdisplay_file *context = file->private_data;
	struct usbdisplay_device *udev = context->udev;
	struct usbdisplay_device_info info;
	long result = 0;

	if (command != USBDISPLAY_IOCTL_GET_INFO) {
		result = -ENOTTY;
	} else {
		memset(&info, 0, sizeof(info));
		mutex_lock(&udev->publish_lock);
		info.abi_version = USBDISPLAY_ABI_VERSION;
		info.width = width;
		info.height = height;
		info.slot_count = USBDISPLAY_SLOT_COUNT;
		info.slot_bytes = (uint32_t)udev->slot_bytes;
		info.map_bytes = (uint32_t)udev->map_bytes;
		info.sequence = udev->sequence;
		mutex_unlock(&udev->publish_lock);
		if (copy_to_user((void __user *)argument, &info, sizeof(info)) != 0) {
			result = -EFAULT;
		}
	}

	return result;
}

static int usbdisplay_stream_mmap(struct file *file,
				  struct vm_area_struct *vma)
{
	struct usbdisplay_file *context = file->private_data;
	struct usbdisplay_device *udev = context->udev;
	size_t requested = vma->vm_end - vma->vm_start;
	int result = 0;

	if ((vma->vm_flags & VM_WRITE) != 0) {
		result = -EPERM;
	} else if (requested != udev->map_bytes || vma->vm_pgoff != 0) {
		result = -EINVAL;
	} else {
		vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
		result = remap_vmalloc_range(vma, udev->frame_ring, 0);
	}

	return result;
}

static const struct file_operations usbdisplay_stream_fops = {
	.owner = THIS_MODULE,
	.open = usbdisplay_stream_open,
	.release = usbdisplay_stream_release,
	.read = usbdisplay_stream_read,
	.poll = usbdisplay_stream_poll,
	.unlocked_ioctl = usbdisplay_stream_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = usbdisplay_stream_ioctl,
#endif
	.mmap = usbdisplay_stream_mmap,
	.llseek = no_llseek,
};

static void usbdisplay_vfree(void *data)
{
	vfree(data);
}

static void usbdisplay_misc_deregister(void *data)
{
	misc_deregister(data);
}

static void usbdisplay_drm_unregister(void *data)
{
	struct usbdisplay_device *udev = data;

	drm_atomic_helper_shutdown(udev->tinydrm.drm);
	drm_dev_unregister(udev->tinydrm.drm);
}

static void usbdisplay_fbdev_unregister(void *data)
{
	struct fb_info *info = data;

	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	framebuffer_release(info);
}

static int usbdisplay_register_fbdev(struct device *device,
				     struct usbdisplay_device *udev)
{
	struct fb_info *info = NULL;
	unsigned int stride = width * (USBDISPLAY_FB_BPP / 8);
	int result = 0;

	udev->fb_bytes = PAGE_ALIGN((size_t)stride * height);
	udev->fb_memory = vzalloc(udev->fb_bytes);
	if (udev->fb_memory == NULL) {
		result = -ENOMEM;
	} else {
		result = devm_add_action(device, usbdisplay_vfree,
					 udev->fb_memory);
		if (result != 0) {
			vfree(udev->fb_memory);
			udev->fb_memory = NULL;
		}
	}

	if (result == 0) {
		info = framebuffer_alloc(0, device);
		if (info == NULL) {
			result = -ENOMEM;
		} else {
			udev->fb_info = info;
			info->par = udev;
			info->fbops = &usbdisplay_fbdev_ops;
			info->flags = FBINFO_DEFAULT | FBINFO_VIRTFB;
			info->screen_buffer = udev->fb_memory;
			info->screen_size = udev->fb_bytes;
			info->pseudo_palette = udev->pseudo_palette;
			strncpy(info->fix.id, USBDISPLAY_DRIVER_NAME,
				sizeof(info->fix.id) - 1);
			info->fix.type = FB_TYPE_PACKED_PIXELS;
			info->fix.visual = FB_VISUAL_TRUECOLOR;
			info->fix.line_length = stride;
			info->fix.smem_len = udev->fb_bytes;
			info->fix.accel = FB_ACCEL_NONE;
			info->var.xres = width;
			info->var.yres = height;
			info->var.xres_virtual = width;
			info->var.yres_virtual = height;
			info->var.bits_per_pixel = USBDISPLAY_FB_BPP;
			info->var.red.offset = 16;
			info->var.red.length = 8;
			info->var.green.offset = 8;
			info->var.green.length = 8;
			info->var.blue.offset = 0;
			info->var.blue.length = 8;
			info->var.transp.offset = 24;
			info->var.transp.length = 0;
			info->var.activate = FB_ACTIVATE_NOW;
			info->var.height = -1;
			info->var.width = -1;
			info->fbdefio = &udev->fb_deferred;
			udev->fb_deferred.delay = max_t(unsigned long, 1, HZ / 30);
			udev->fb_deferred.deferred_io =
				usbdisplay_fbdev_deferred_io;
			fb_deferred_io_init(info);
			usbdisplay_fbdev_ops.fb_mmap = usbdisplay_fbdev_mmap;
			result = register_framebuffer(info);
			if (result != 0) {
				fb_deferred_io_cleanup(info);
				framebuffer_release(info);
				udev->fb_info = NULL;
			}
		}
	}

	if (result == 0) {
		result = devm_add_action(device, usbdisplay_fbdev_unregister,
					 info);
		if (result != 0) {
			usbdisplay_fbdev_unregister(info);
			udev->fb_info = NULL;
		}
	}

	return result;
}

/* INITIAL 帧是无业务应用状态信号，具体 Splash 由用户态守护进程绘制。 */
static int usbdisplay_publish_initial(struct usbdisplay_device *udev)
{
	void *blank;
	unsigned int stride = width * 4;
	int result;

	blank = vzalloc((size_t)stride * height);
	if (blank == NULL) {
		result = -ENOMEM;
	} else {
		result = usbdisplay_publish(udev, blank, width, height, stride,
					    USBDISPLAY_FORMAT_XRGB8888, NULL, 0,
					    USBDISPLAY_SOURCE_INITIAL);
		vfree(blank);
	}

	return result;
}

static int usbdisplay_probe(struct platform_device *platform)
{
	static const uint32_t formats[] = {
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_RGB565,
	};
	struct device *device = &platform->dev;
	struct usbdisplay_device *udev;
	size_t maximum_stride;
	int result = 0;

	if (width == 0 || height == 0 || width > USBDISPLAY_MAX_WIDTH ||
	    height > USBDISPLAY_MAX_HEIGHT) {
		result = -EINVAL;
	} else {
		udev = devm_kzalloc(device, sizeof(*udev), GFP_KERNEL);
		if (udev == NULL) {
			result = -ENOMEM;
		} else {
			platform_set_drvdata(platform, udev);
			mutex_init(&udev->publish_lock);
			init_waitqueue_head(&udev->update_wait);
			atomic_set(&udev->consumer_open, 0);
			atomic_set(&udev->producer_open, 0);
			udev->held_slot = -1;

			maximum_stride = ALIGN((size_t)width * 4, 256);
			udev->slot_bytes = PAGE_ALIGN(maximum_stride * height);
			udev->map_bytes = udev->slot_bytes * USBDISPLAY_SLOT_COUNT;
			udev->frame_ring = vmalloc_user(udev->map_bytes);
			if (udev->frame_ring == NULL) {
				result = -ENOMEM;
			} else {
				result = devm_add_action(device, usbdisplay_vfree,
							 udev->frame_ring);
			}

			if (result == 0) {
				result = devm_tinydrm_init(device, &udev->tinydrm,
							  &usbdisplay_fb_funcs,
							  &usbdisplay_drm_driver);
			}
			if (result == 0) {
				udev->tinydrm.drm->dev_private = udev;
				udev->tinydrm.drm->mode_config.preferred_depth = 32;
				memset(&udev->mode, 0, sizeof(udev->mode));
				udev->mode.hdisplay = width;
				udev->mode.hsync_start = width;
				udev->mode.hsync_end = width;
				udev->mode.htotal = width;
				udev->mode.vdisplay = height;
				udev->mode.vsync_start = height;
				udev->mode.vsync_end = height;
				udev->mode.vtotal = height;
				udev->mode.type = DRM_MODE_TYPE_DRIVER |
						  DRM_MODE_TYPE_PREFERRED;
				udev->mode.clock = 1;
				result = tinydrm_display_pipe_init(&udev->tinydrm,
						&usbdisplay_pipe_funcs,
						DRM_MODE_CONNECTOR_VIRTUAL,
						formats, ARRAY_SIZE(formats),
						&udev->mode, 0);
			}
			if (result == 0) {
				drm_mode_config_reset(udev->tinydrm.drm);
				result = drm_dev_register(udev->tinydrm.drm, 0);
			}
			if (result == 0) {
				result = devm_add_action(device,
						 usbdisplay_drm_unregister, udev);
				if (result != 0) {
					usbdisplay_drm_unregister(udev);
				}
			}
			if (result == 0) {
				result = usbdisplay_register_fbdev(device, udev);
			}
			if (result == 0) {
				udev->miscdev.minor = MISC_DYNAMIC_MINOR;
				udev->miscdev.name = "usbdisplay0";
				udev->miscdev.fops = &usbdisplay_stream_fops;
				udev->miscdev.parent = device;
				result = misc_register(&udev->miscdev);
			}
			if (result == 0) {
				result = devm_add_action(device,
							 usbdisplay_misc_deregister,
							 &udev->miscdev);
			}
			if (result == 0) {
				result = usbdisplay_publish_initial(udev);
			}
			if (result == 0) {
				dev_info(device,
					 "registered %ux%u DRM, fb%d and /dev/%s\n",
					 width, height, udev->fb_info->node,
					 udev->miscdev.name);
			}
		}
	}

	return result;
}

static struct platform_driver usbdisplay_platform_driver = {
	.driver = {
		.name = USBDISPLAY_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
	.probe = usbdisplay_probe,
};

static int __init usbdisplay_init(void)
{
	struct platform_device *platform;
	int result;

	result = platform_driver_register(&usbdisplay_platform_driver);
	if (result == 0) {
		platform = platform_device_alloc(USBDISPLAY_DRIVER_NAME, 0);
		if (platform == NULL) {
			result = -ENOMEM;
		} else {
			platform->dev.coherent_dma_mask = DMA_BIT_MASK(32);
			platform->dev.dma_mask = &platform->dev.coherent_dma_mask;
			result = platform_device_add(platform);
			if (result != 0) {
				platform_device_put(platform);
			} else {
				usbdisplay_platform_device = platform;
			}
		}
		if (result != 0) {
			platform_driver_unregister(&usbdisplay_platform_driver);
		}
	}

	return result;
}

static void __exit usbdisplay_exit(void)
{
	platform_device_unregister(usbdisplay_platform_device);
	platform_driver_unregister(&usbdisplay_platform_driver);
}

module_init(usbdisplay_init);
module_exit(usbdisplay_exit);

MODULE_AUTHOR("IoTSharp contributors");
MODULE_DESCRIPTION(USBDISPLAY_DRIVER_DESC);
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0-alpha.1");
