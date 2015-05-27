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

static pthread_mutex_t serializer[1] = { PTHREAD_MUTEX_INITIALIZER };

static inline void
serialized_start(const char *func)
{
	pthread_mutex_lock(serializer);
}

static inline void
serialized_stop(void)
{
	pthread_mutex_unlock(serializer);
}

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

	if (!strcmp(path, "/dev/galcore")) {
		galcore = 1;
		serialized_start(__func__);
	}

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

	if (galcore)
		serialized_stop();

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

	if (fd == dev_galcore_fd)
	    	serialized_start(__func__);

	if (!orig_close)
		orig_close = libc_dlsym(__func__);

	if (fd == dev_galcore_fd) {
		wrap_log("/* CLOSE */\n");
		dev_galcore_fd = -1;
	}

	ret = orig_close(fd);

	if (fd == dev_galcore_fd)
		serialized_stop();

	return ret;
}
