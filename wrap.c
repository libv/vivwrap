/*
 * Copyright (c) 2011-2015 Luc Verhaegen <libv@skynet.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the license, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
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
#include <stdint.h>

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

	fflush(viv_wrap_log);

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

#include "gc_hal_base.h"
#include "gc_hal_profiler.h"
#include "gc_hal_driver.h"

typedef struct _DRIVER_ARGS
{
    gctUINT64               InputBuffer;
    gctUINT64               InputBufferSize;
    gctUINT64               OutputBuffer;
    gctUINT64               OutputBufferSize;
}
DRIVER_ARGS;

struct {
	int command;
	char *name;
} command_table[] = {
	{gcvHAL_QUERY_VIDEO_MEMORY, "QUERY_VIDEO_MEMORY"},
	{gcvHAL_QUERY_CHIP_IDENTITY, "QUERY_CHIP_IDENTITY"},
	{gcvHAL_ALLOCATE_NON_PAGED_MEMORY, "ALLOCATE_NON_PAGED_MEMORY"},
	{gcvHAL_FREE_NON_PAGED_MEMORY, "FREE_NON_PAGED_MEMORY"},
	{gcvHAL_ALLOCATE_CONTIGUOUS_MEMORY, "ALLOCATE_CONTIGUOUS_MEMORY"},
	{gcvHAL_FREE_CONTIGUOUS_MEMORY, "FREE_CONTIGUOUS_MEMORY"},
	{gcvHAL_ALLOCATE_VIDEO_MEMORY, "ALLOCATE_VIDEO_MEMORY"},
	{gcvHAL_ALLOCATE_LINEAR_VIDEO_MEMORY, "ALLOCATE_LINEAR_VIDEO_MEMORY"},
	{gcvHAL_FREE_VIDEO_MEMORY, "FREE_VIDEO_MEMORY"},
	{gcvHAL_MAP_MEMORY, "MAP_MEMORY"},
	{gcvHAL_UNMAP_MEMORY, "UNMAP_MEMORY"},
	{gcvHAL_MAP_USER_MEMORY, "MAP_USER_MEMORY"},
	{gcvHAL_UNMAP_USER_MEMORY, "UNMAP_USER_MEMORY"},
	{gcvHAL_LOCK_VIDEO_MEMORY, "LOCK_VIDEO_MEMORY"},
	{gcvHAL_UNLOCK_VIDEO_MEMORY, "UNLOCK_VIDEO_MEMORY"},
	{gcvHAL_EVENT_COMMIT, "EVENT_COMMIT"},
	{gcvHAL_USER_SIGNAL, "USER_SIGNAL"},
	{gcvHAL_SIGNAL, "SIGNAL"},
	{gcvHAL_WRITE_DATA, "WRITE_DATA"},
	{gcvHAL_COMMIT, "COMMIT"},
	{gcvHAL_STALL, "STALL"},
	{gcvHAL_READ_REGISTER, "READ_REGISTER"},
	{gcvHAL_WRITE_REGISTER, "WRITE_REGISTER"},
	{gcvHAL_GET_PROFILE_SETTING, "GET_PROFILE_SETTING"},
	{gcvHAL_SET_PROFILE_SETTING, "SET_PROFILE_SETTING"},
	{gcvHAL_READ_ALL_PROFILE_REGISTERS, "READ_ALL_PROFILE_REGISTERS"},
	{gcvHAL_PROFILE_REGISTERS_2D, "PROFILE_REGISTERS_2D"},
#if VIVANTE_PROFILER_PERDRAW
	{gcvHAL_READ_PROFILER_REGISTER_SETTING, "READ_PROFILER_REGISTER_SETTING"},
#endif
	{gcvHAL_SET_POWER_MANAGEMENT_STATE, "SET_POWER_MANAGEMENT_STATE"},
	{gcvHAL_QUERY_POWER_MANAGEMENT_STATE, "QUERY_POWER_MANAGEMENT_STATE"},
	{gcvHAL_GET_BASE_ADDRESS, "GET_BASE_ADDRESS"},
	{gcvHAL_SET_IDLE, "SET_IDLE"},
	{gcvHAL_QUERY_KERNEL_SETTINGS, "QUERY_KERNEL_SETTINGS"},
	{gcvHAL_RESET, "RESET"},
	{gcvHAL_MAP_PHYSICAL, "MAP_PHYSICAL"},
	{gcvHAL_DEBUG, "DEBUG"},
	{gcvHAL_CACHE, "CACHE"},
	{gcvHAL_TIMESTAMP, "TIMESTAMP"},
	{gcvHAL_DATABASE, "DATABASE"},
	{gcvHAL_VERSION, "VERSION"},
	{gcvHAL_CHIP_INFO, "CHIP_INFO"},
	{gcvHAL_ATTACH, "ATTACH"},
	{gcvHAL_DETACH, "DETACH"},
	{gcvHAL_COMPOSE, "COMPOSE"},
	{gcvHAL_SET_TIMEOUT, "SET_TIMEOUT"},
	{gcvHAL_GET_FRAME_INFO, "GET_FRAME_INFO"},
	{gcvHAL_GET_SHARED_INFO, "GET_SHARED_INFO"},
	{gcvHAL_SET_SHARED_INFO, "SET_SHARED_INFO"},
	{gcvHAL_QUERY_COMMAND_BUFFER, "QUERY_COMMAND_BUFFER"},
	{gcvHAL_COMMIT_DONE, "COMMIT_DONE"},
	{gcvHAL_DUMP_GPU_STATE, "DUMP_GPU_STATE"},
	{gcvHAL_DUMP_EVENT, "DUMP_EVENT"},
	{gcvHAL_ALLOCATE_VIRTUAL_COMMAND_BUFFER, "ALLOCATE_VIRTUAL_COMMAND_BUFFER"},
	{gcvHAL_FREE_VIRTUAL_COMMAND_BUFFER, "FREE_VIRTUAL_COMMAND_BUFFER"},
	{gcvHAL_SET_FSCALE_VALUE, "SET_FSCALE_VALUE"},
	{gcvHAL_GET_FSCALE_VALUE, "GET_FSCALE_VALUE"},
	{gcvHAL_QUERY_RESET_TIME_STAMP, "QUERY_RESET_TIME_STAMP"},
	{gcvHAL_SYNC_POINT, "SYNC_POINT"},
	{gcvHAL_CREATE_NATIVE_FENCE, "CREATE_NATIVE_FENCE"},
	{gcvHAL_VIDMEM_DATABASE, "VIDMEM_DATABASE"},
};

static int
galcore_ioctl(int request, void *data)
{
	DRIVER_ARGS *args = data;
	gcsHAL_INTERFACE *input, *output;
	char *command_name, *hardware;
	int ret;

	if (request != IOCTL_GCHAL_INTERFACE) {
		fprintf(stderr, "%s: wrong request: 0x%X\n", __func__,
			(unsigned int) request);
		return -1;
	}

	if (!data) {
		fprintf(stderr, "%s: no data???\n", __func__);
		return -1;
	}

	if ((args->InputBufferSize  != sizeof(gcsHAL_INTERFACE)) ||
	    (args->OutputBufferSize != sizeof(gcsHAL_INTERFACE))) {
		fprintf(stderr, "%s: wrong data sizes: %lld/%lld\n", __func__,
			args->InputBufferSize, args->OutputBufferSize);
		return -1;
	}

	input = (gcsHAL_INTERFACE *) ((uint32_t) args->InputBuffer);
	output = (gcsHAL_INTERFACE *) ((uint32_t) args->OutputBuffer);

	if (!input) {
		fprintf(stderr, "%s: missing input\n", __func__);
		return -1;
	}

	if (!output) {
		fprintf(stderr, "%s: missing output\n", __func__);
		return -1;
	}

	command_name = command_table[input->command].name;

	if (input != output) {
		fprintf(stderr, "%s: input buffer does not match output.\n",
			command_name);
		wrap_log("%s: input buffer does not match output.\n",
			 command_name);
		return -1;
	}

	switch(input->hardwareType) {
	case 1:
		hardware = "3D";
		break;
	case 2:
		hardware = "2D";
		break;
	case 3:
		hardware = "2D/3D";
		break;
	case 4:
		hardware = "VG";
		break;
	default:
		fprintf(stderr, "%s: unknown hardware type %d\n",
			command_name, input->hardwareType);
		wrap_log("%s: unknown hardware type %d\n",
			command_name, input->hardwareType);
		return -1;
	}

	wrap_log("%s(%s)\n", command_name, hardware);

	ret = orig_ioctl(dev_galcore_fd, request, data);

	wrap_log("%s(%s) = %d\n", command_name, hardware, ret);

	return ret;
}
