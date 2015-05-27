/*
 * Copyright (c) 2011-2015 Luc Verhaegen <libv@skynet.be>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <asm/ioctl.h>

/*
 *
 * Basic log writing infrastructure.
 *
 */
FILE *viv_wrap_log;
int frame_count;

void
wrap_log_open(void)
{
	char *filename;

	if (viv_wrap_log)
		return;

	filename = getenv("VIV_WRAP_LOG");
	if (!filename)
		filename = "/home/root/viv_wrap.log";

	viv_wrap_log = fopen(filename, "w");
	if (!viv_wrap_log) {
		fprintf(stderr, "Error: failed to open wrap log %s: %s\n",
			filename, strerror(errno));
		viv_wrap_log = stdout;
		printf("viv_wrap: dumping to stdout.\n");
	} else
		printf("viv_wrap: dumping to %s.\n", filename);
}

int
wrap_log(const char *format, ...)
{
	va_list args;
	int ret;

	wrap_log_open();

	va_start(args, format);
	ret = vfprintf(viv_wrap_log, format, args);
	va_end(args);

	return ret;
}

/*
 * Wrap around the libc calls that are crucial for capturing our
 * command stream, namely, open, ioctl, and mmap.
 */
static void *libc_dl;

static int
libc_dlopen(void)
{
	libc_dl = dlopen("libc.so.6", RTLD_LAZY);
	if (!libc_dl) {
		printf("Failed to dlopen %s: %s\n",
		       "libc.so", dlerror());
		exit(-1);
	}

	return 0;
}

static void *
libc_dlsym(const char *name)
{
	void *func;

	if (!libc_dl)
		libc_dlopen();

	func = dlsym(libc_dl, name);

	if (!func) {
		printf("Failed to find %s in %s: %s\n",
		       name, "libc.so", dlerror());
		exit(-1);
	}

	return func;
}

static int dev_galcore_fd;

/*
 *
 */
static int (*orig_open)(const char* path, int mode, ...);

int
open(const char* path, int flags, ...)
{
	mode_t mode = 0;
	int ret;
	int galcore = 0;

	if (!strcmp(path, "/dev/galcore"))
		galcore = 1;

	if (!orig_open)
		orig_open = libc_dlsym(__func__);

	if (flags & O_CREAT) {
		va_list  args;


		va_start(args, flags);
		mode = (mode_t) va_arg(args, int);
		va_end(args);

		ret = orig_open(path, flags, mode);
	} else {
		ret = orig_open(path, flags);

		if (ret != -1) {
			if (galcore) {
				dev_galcore_fd = ret;
				wrap_log("/* OPEN */\n");
			}
		}
	}

	return ret;
}

/*
 *
 */
static int (*orig_close)(int fd);

int
close(int fd)
{
	int ret;

	if (!orig_close)
		orig_close = libc_dlsym(__func__);

	if (fd == dev_galcore_fd) {
		wrap_log("/* CLOSE */\n");
		dev_galcore_fd = -1;
	}

	ret = orig_close(fd);

	return ret;
}

/*
 *
 */
static int galcore_ioctl(int request, void *data);
static int (*orig_ioctl)(int fd, unsigned long request, ...);

int
ioctl(int fd, unsigned long request, ...)
{
	int ioc_size = _IOC_SIZE(request);
	int ret;

	if (!orig_ioctl)
		orig_ioctl = libc_dlsym(__func__);

	/* Vivante is soo broken. */
	if (ioc_size || (fd == dev_galcore_fd)) {
		va_list args;
		void *ptr;

		va_start(args, request);
		ptr = va_arg(args, void *);
		va_end(args);

		if (fd == dev_galcore_fd)
			ret = galcore_ioctl(request, ptr);
		else
			ret = orig_ioctl(fd, request, ptr);
	} else {
		ret = orig_ioctl(fd, request);
	}

	return ret;
}

char *
ioctl_dir_string(int request)
{
	switch (_IOC_DIR(request)) {
	default: /* cannot happen */
	case 0x00:
		return "_IO";
	case 0x01:
		return "_IOW";
	case 0x02:
		return "_IOR";
	case 0x03:
		return "_IOWR";
	}
}


static int
galcore_ioctl(int request, void *data)
{
	int ioc_type = _IOC_TYPE(request);
	int ioc_nr = _IOC_NR(request);
	char *ioc_string = ioctl_dir_string(request);
	//int i;
	int ret;

#if 0
	if (!ioctl_table) {
		if ((ioc_type == GALCORE_IOC_CORE_BASE) &&
		    (ioc_nr == _GALCORE_UK_GET_API_VERSION_R3P1))
			ioctl_table = dev_galcore_ioctls_r3p1;
		else
			ioctl_table = dev_galcore_ioctls;
	}

	for (i = 0; ioctl_table[i].name; i++) {
		if ((ioctl_table[i].type == ioc_type) &&
		    (ioctl_table[i].nr == ioc_nr)) {
			ioctl = &ioctl_table[i];
			break;
		}
	}

	if (!ioctl) {
		char *name = ioc_type_name(ioc_type);

		if (name)
			wrap_log("/* Error: No galcore ioctl wrapping implemented for %s:%02X */\n",
				 name, ioc_nr);
		else
			wrap_log("/* Error: No galcore ioctl wrapping implemented for %02X:%02X */\n",
				 ioc_type, ioc_nr);

	}

	if (ioctl && ioctl->pre)
		ioctl->pre(data);
#endif

	if (data)
		ret = orig_ioctl(dev_galcore_fd, request, data);
	else
		ret = orig_ioctl(dev_galcore_fd, request);

#if 0
	if (ret == -EPERM) {
		if ((ioc_type == GALCORE_IOC_CORE_BASE) &&
		    (ioc_nr == _GALCORE_UK_GET_API_VERSION))
			ioctl_table = dev_galcore_ioctls_r3p1;
	}

	if (ioctl && !ioctl->pre && !ioctl->post) {
		if (data)
			wrap_log("/* IOCTL %s(%s) %p = %d */\n",
				 ioc_string, ioctl->name, data, ret);
		else
			wrap_log("/* IOCTL %s(%s) = %d */\n",
				 ioc_string, ioctl->name, ret);
	}

	if (ioctl && ioctl->post)
		ioctl->post(data, ret);
#else
	if (data)
		wrap_log("/* IOCTL %s(%d, %d) %p = %d */\n",
			 ioc_string, ioc_type, ioc_nr, data, ret);
	else
		wrap_log("/* IOCTL %s(%d, %d) = %d */\n",
			 ioc_string, ioc_type, ioc_nr, ret);
#endif

	return ret;
}
