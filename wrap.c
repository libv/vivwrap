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
static pthread_mutex_t wrap_log_mutex[1] = { PTHREAD_MUTEX_INITIALIZER };
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

void
wrap_log_start(void)
{
	wrap_log_open();
	pthread_mutex_lock(wrap_log_mutex);
}

void
wrap_log_stop(void)
{
	wrap_log_open();

	fflush(viv_wrap_log);
	pthread_mutex_unlock(wrap_log_mutex);
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
			if (galcore)
				dev_galcore_fd = ret;
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

	if (fd == dev_galcore_fd)
		dev_galcore_fd = -1;

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

#define gcdENABLE_VG 1
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

static const char *
viv_hardware_type(int type)
{
	switch(type) {
	case 1:
		return "3D";
	case 2:
		return "2D";
	case 3:
		return "2D/3D";
	case 4:
		return "VG";
	default:
		return NULL;
	}
}

static int
hook_unknown_pre(const char *command, int hardware, void *data)
{
	return -1;
}

static int
hook_unknown_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	return -1;
}

static int
hook_empty_pre(const char *command, int hardware, void *data)
{
	return 0;
}

#if 0
static int
hook_empty_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	return 0;
}
#endif

static int
hook_GetBaseAddress_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_GET_BASE_ADDRESS *address = data;

	wrap_log_start();

	wrap_log("%s = 0x%08X;\n", command, address->baseAddress);

	wrap_log_stop();

	return 0;
}

static int
hook_Version_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_VERSION *version = data;

	wrap_log_start();

	wrap_log("%s = %d.%d.%d.%d;\n", command, version->major, version->minor,
		 version->patch, version->build);

	wrap_log_stop();

	return 0;
}

static int
hook_ChipInfo_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_CHIP_INFO *info = data;
	int i;

	wrap_log_start();

	wrap_log("%s[%d] = {\n", command, info->count);
	for (i = 0; i < info->count; i++) {
		const char *name = viv_hardware_type(info->types[i]);

		if (name)
			wrap_log("\t%s, /* %d */\n", name, i);
		else
			wrap_log("\tNULL, /* %d */\n", name, i);
	}

	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_QueryVideoMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_QUERY_VIDEO_MEMORY *memory = data;

	wrap_log_start();

	wrap_log("%s = {\n", command);
	wrap_log("\t.internalPhysical = 0x%08lX,\n", memory->internalPhysical);
	wrap_log("\t.internalSize = %lld,\n", memory->internalSize);
	wrap_log("\t.externalPhysical = 0x%08lX,\n", memory->externalPhysical);
	wrap_log("\t.externalSize = %lld,\n", memory->externalSize);
	wrap_log("\t.contiguousPhysical = 0x%08lX,\n", memory->contiguousPhysical);
	wrap_log("\t.contiguousSize = %lld,\n", memory->contiguousSize);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_QueryChipIdentity_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_QUERY_CHIP_IDENTITY *identity = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.chipModel = %d,\n", identity->chipModel);
	wrap_log("\t.chipRevision = 0x%08lX,\n", identity->chipRevision);
	wrap_log("\t.chipFeatures = 0x%08lX,\n", identity->chipFeatures);
	wrap_log("\t.chipMinorFeatures = 0x%08lX,\n", identity->chipMinorFeatures);
	wrap_log("\t.chipMinorFeatures1 = 0x%08lX,\n", identity->chipMinorFeatures1);
	wrap_log("\t.chipMinorFeatures2 = 0x%08lX,\n", identity->chipMinorFeatures2);
	wrap_log("\t.chipMinorFeatures3 = 0x%08lX,\n", identity->chipMinorFeatures3);
	wrap_log("\t.chipMinorFeatures4 = 0x%08lX,\n", identity->chipMinorFeatures4);
	wrap_log("\t.streamCount = 0x%08lX,\n", identity->streamCount);
	wrap_log("\t.registerMax = 0x%08lX,\n", identity->registerMax);
	wrap_log("\t.threadCount = 0x%08lX,\n", identity->threadCount);
	wrap_log("\t.shaderCoreCount = 0x%08lX,\n", identity->shaderCoreCount);
	wrap_log("\t.vertexCacheSize = 0x%08lX,\n", identity->vertexCacheSize);
	wrap_log("\t.vertexOutputBufferSize = 0x%08lX,\n", identity->vertexOutputBufferSize);
	wrap_log("\t.pixelPipes = 0x%08lX,\n", identity->pixelPipes);
	wrap_log("\t.instructionCount = 0x%08lX,\n", identity->instructionCount);
	wrap_log("\t.numConstants = 0x%08lX,\n", identity->numConstants);
	wrap_log("\t.bufferSize = 0x%08lX,\n", identity->bufferSize);
	wrap_log("\t.varyingsCount = 0x%08lX,\n", identity->varyingsCount);
	wrap_log("\t.superTileMode = 0x%08lX,\n", identity->superTileMode);
	wrap_log("\t.chip2DControl = 0x%08lX,\n", identity->chip2DControl);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_Attach_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_ATTACH *attach = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.context = 0x%08lX,\n", attach->context);
	wrap_log("\t.stateCount = %lld,\n", attach->stateCount);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_AllocateContiguousMemory_pre(const char *command, int hardware, void *data)
{
	struct  _gcsHAL_ALLOCATE_CONTIGUOUS_MEMORY *alloc = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.bytes = 0x%08llX,\n", alloc->bytes);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_AllocateContiguousMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct  _gcsHAL_ALLOCATE_CONTIGUOUS_MEMORY *alloc = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.bytes = 0x%08llX,\n", alloc->bytes);
	wrap_log("\t.address = 0x%08lX,\n", alloc->address);
	wrap_log("\t.physical = 0x%08lX,\n", alloc->physical);
	wrap_log("\t.logical = 0x%08llX,\n", alloc->logical);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static const char *
viv_user_signal_name(gceUSER_SIGNAL_COMMAND_CODES command)
{
	switch(command) {
	case gcvUSER_SIGNAL_CREATE:
		return "CREATE";
	case gcvUSER_SIGNAL_DESTROY:
		return "DESTROY";
	case gcvUSER_SIGNAL_SIGNAL:
		return "SIGNAL";
	case gcvUSER_SIGNAL_WAIT:
		return "WAIT";
	case gcvUSER_SIGNAL_MAP:
		return "MAP";
	case gcvUSER_SIGNAL_UNMAP:
		return "UNMAP";
	default:
		return NULL;
	}
}

static int
hook_UserSignal_pre(const char *command, int hardware, void *data)
{
	struct  _gcsHAL_USER_SIGNAL *signal = data;
	const char *name = viv_hardware_type(hardware);
	const char *signal_command = viv_user_signal_name(signal->command);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.command = %s,\n", signal_command);
	wrap_log("\t.id = 0x%08lX,\n", signal->id);
	wrap_log("\t.manualReset = %d,\n", signal->manualReset);
	wrap_log("\t.wait = 0x%08lX,\n", signal->wait);
	wrap_log("\t.state = %d,\n", signal->state);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_UserSignal_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct  _gcsHAL_USER_SIGNAL *signal = data;
	const char *name = viv_hardware_type(hardware);
	const char *signal_command = viv_user_signal_name(signal->command);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.command = %s,\n", signal_command);
	wrap_log("\t.id = 0x%08lX,\n", signal->id);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_QueryCommandBuffer_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_QUERY_COMMAND_BUFFER *query = data;
	struct _gcsCOMMAND_BUFFER_INFO *info = &query->information;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {{\n", command, name);
	wrap_log("\t.feBufferInt = 0x%08lX,\n", info->feBufferInt);
	wrap_log("\t.tsOverflowInt = 0x%08lX,\n", info->tsOverflowInt);
	wrap_log("\t.addressMask = 0x%08lX,\n", info->addressMask);
	wrap_log("\t.addressAlignment = 0x%08lX,\n", info->addressAlignment);
	wrap_log("\t.commandAlignment = 0x%08lX,\n", info->commandAlignment);
	wrap_log("\t.stateCommandSize = 0x%08lX,\n", info->stateCommandSize);
	wrap_log("\t.restartCommandSize = 0x%08lX,\n", info->restartCommandSize);
	wrap_log("\t.fetchCommandSize = 0x%08lX,\n", info->fetchCommandSize);
	wrap_log("\t.callCommandSize = 0x%08lX,\n", info->callCommandSize);
	wrap_log("\t.returnCommandSize = 0x%08lX,\n", info->returnCommandSize);
	wrap_log("\t.eventCommandSize = 0x%08lX,\n", info->eventCommandSize);
	wrap_log("\t.endCommandSize = 0x%08lX,\n", info->endCommandSize);
	wrap_log("\t.staticTailSize = 0x%08lX,\n", info->staticTailSize);
	wrap_log("\t.dynamicTailSize = 0x%08lX,\n", info->dynamicTailSize);
	wrap_log("}};\n");

	wrap_log_stop();

	return 0;
}

static const char *
viv_surf_type(gceSURF_TYPE type)
{
	switch(type) {
	case gcvSURF_TYPE_UNKNOWN:
		return "TYPE_UNKNOWN";
	case gcvSURF_INDEX:
		return "INDEX";
	case gcvSURF_VERTEX:
		return "VERTEX";
	case gcvSURF_TEXTURE:
		return "TEXTURE";
	case gcvSURF_RENDER_TARGET:
		return "RENDER_TARGET";
	case gcvSURF_DEPTH:
		return "DEPTH";
	case gcvSURF_BITMAP:
		return "BITMAP";
	case gcvSURF_TILE_STATUS:
		return "TILE_STATUS";
	case gcvSURF_MASK:
		return "MASK";
	case gcvSURF_SCISSOR:
		return "SCISSOR";
	case gcvSURF_HIERARCHICAL_DEPTH:
		return "HIERARCHICAL_DEPTH";
	case gcvSURF_NUM_TYPES:
		return "NUM_TYPES";
	case gcvSURF_NO_TILE_STATUS:
		return "NO_TILE_STATUS";
	case gcvSURF_NO_VIDMEM:
		return "NO_VIDMEM";
	case gcvSURF_CACHEABLE:
		return "CACHEABLE";
	case gcvSURF_FLIP:
		return "FLIP";
	case gcvSURF_TILE_STATUS_DIRTY:
		return "TILE_STATUS_DIRTY";
	case gcvSURF_LINEAR:
		return "LINEAR";
	case gcvSURF_VG:
		return "VG";
	case gcvSURF_TEXTURE_LINEAR:
		return "TEXTURE_LINEAR";
	case gcvSURF_RENDER_TARGET_NO_TILE_STATUS:
		return "RENDER_TARGET_NO_TILE_STATUS";
	case gcvSURF_RENDER_TARGET_TS_DIRTY:
		return "RENDER_TARGET_TS_DIRTY";
	case gcvSURF_DEPTH_NO_TILE_STATUS:
		return "DEPTH_NO_TILE_STATUS";
	case gcvSURF_DEPTH_TS_DIRTY:
		return "DEPTH_TS_DIRTY";
	case gcvSURF_BITMAP_NO_VIDMEM:
		return "BITMAP_NO_VIDMEM";
	case gcvSURF_TEXTURE_NO_VIDMEM:
		return "TEXTURE_NO_VIDMEM";
	case gcvSURF_CACHEABLE_BITMAP_NO_VIDMEM:
		return "CACHEABLE_BITMAP_NO_VIDMEM";
	case gcvSURF_CACHEABLE_BITMAP:
		return "CACHEABLE_BITMAP";
	case gcvSURF_FLIP_BITMAP:
		return "FLIP_BITMAP";
	default:
		return NULL;
	}
}

static const char *
viv_pool(gcePOOL pool)
{
	switch(pool) {
	case gcvPOOL_UNKNOWN:
		return "UNKNOWN";
	case gcvPOOL_DEFAULT:
		return "DEFAULT";
	case gcvPOOL_LOCAL:
		return "LOCAL";
	case gcvPOOL_LOCAL_INTERNAL:
		return "LOCAL_INTERNAL";
	case gcvPOOL_LOCAL_EXTERNAL:
		return "LOCAL_EXTERNAL";
	case gcvPOOL_UNIFIED:
		return "UNIFIED";
	case gcvPOOL_SYSTEM:
		return "SYSTEM";
	case gcvPOOL_VIRTUAL:
		return "VIRTUAL";
	case gcvPOOL_USER:
		return "USER";
	case gcvPOOL_CONTIGUOUS:
		return "CONTIGUOUS";
	case gcvPOOL_DEFAULT_FORCE_CONTIGUOUS:
		return "DEFAULT_FORCE_CONTIGUOUS";
	case gcvPOOL_DEFAULT_FORCE_CONTIGUOUS_CACHEABLE:
		return "DEFAULT_FORCE_CONTIGUOUS_CACHEABLE";
	default:
		return NULL;
	}
}

static int
hook_AllocateLinearVideoMemory_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_ALLOCATE_LINEAR_VIDEO_MEMORY *alloc = data;
	const char *name = viv_hardware_type(hardware);
	const char *type = viv_surf_type(alloc->type);
	const char *pool = viv_pool(alloc->pool);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.bytes = 0x%08lX,\n", alloc->bytes);
	wrap_log("\t.alignment = 0x%08lX,\n", alloc->alignment);
	wrap_log("\t.type = %s,\n", type);
	wrap_log("\t.pool = %s,\n", pool);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_AllocateLinearVideoMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_ALLOCATE_LINEAR_VIDEO_MEMORY *alloc = data;
	const char *name = viv_hardware_type(hardware);
	const char *pool = viv_pool(alloc->pool);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.bytes = 0x%08lX,\n", alloc->bytes);
	wrap_log("\t.pool = %s,\n", pool);
	wrap_log("\t.node = 0x%08llX,\n", alloc->node);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_LockVideoMemory_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_LOCK_VIDEO_MEMORY *lock = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.node = 0x%08lX,\n", lock->node);
	wrap_log("\t.cacheable = 0x%08lX,\n", lock->cacheable);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_LockVideoMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_LOCK_VIDEO_MEMORY *lock = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.address = 0x%08lX,\n", lock->address);
	wrap_log("\t.memory = 0x%08llX,\n", lock->memory);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_Commit_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_COMMIT *commit = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.queue = 0x%08llX,\n", commit->queue);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_Commit_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_COMMIT *commit = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.queue = 0x%08llX,\n", commit->queue);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_UnlockVideoMemory_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_UNLOCK_VIDEO_MEMORY *unlock = data;
	const char *name = viv_hardware_type(hardware);
	const char *type = viv_surf_type(unlock->type);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.node = 0x%08llX,\n", unlock->node);
	wrap_log("\t.type = %s,\n", type);
	wrap_log("\t.asynchroneous = 0x%08llX,\n", unlock->asynchroneous);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_UnlockVideoMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_UNLOCK_VIDEO_MEMORY *unlock = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.node = 0x%08llX,\n", unlock->node);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_EventCommit_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_EVENT_COMMIT *commit = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.queue = 0x%08llX,\n", commit->queue);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_EventCommit_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_EVENT_COMMIT *commit = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.queue = 0x%08llX,\n", commit->queue);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_Detach_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_DETACH *detach = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.context = 0x%08llX,\n", detach->context);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_Detach_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_DETACH *detach = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.context = 0x%08llX,\n", detach->context);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

static int
hook_FreeVideoMemory_pre(const char *command, int hardware, void *data)
{
	struct _gcsHAL_FREE_VIDEO_MEMORY *free = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.node = 0x%08llX,\n", free->node);
	wrap_log("};\n");

	wrap_log_stop();

	return 0;
}

static int
hook_FreeVideoMemory_post(const char *command, int hardware, void *data, int ioctl_ret)
{
	struct _gcsHAL_FREE_VIDEO_MEMORY *free = data;
	const char *name = viv_hardware_type(hardware);

	wrap_log_start();

	wrap_log("%s(%s) = {\n", command, name);
	wrap_log("\t.node = 0x%08llX,\n", free->node);
	wrap_log("} = %d;\n", ioctl_ret);

	wrap_log_stop();

	return 0;
}

struct {
	int command;
	char *name;
	int (*pre) (const char *command, int hardware, void *data);
	int (*post) (const char *command, int hardware, void *data, int ioctl_ret);
} command_table[] = {
	{gcvHAL_QUERY_VIDEO_MEMORY, "QUERY_VIDEO_MEMORY", hook_empty_pre, hook_QueryVideoMemory_post},
	{gcvHAL_QUERY_CHIP_IDENTITY, "QUERY_CHIP_IDENTITY", hook_empty_pre, hook_QueryChipIdentity_post},
	{gcvHAL_ALLOCATE_NON_PAGED_MEMORY, "ALLOCATE_NON_PAGED_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_FREE_NON_PAGED_MEMORY, "FREE_NON_PAGED_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_ALLOCATE_CONTIGUOUS_MEMORY, "ALLOCATE_CONTIGUOUS_MEMORY", hook_AllocateContiguousMemory_pre, hook_AllocateContiguousMemory_post},
	{gcvHAL_FREE_CONTIGUOUS_MEMORY, "FREE_CONTIGUOUS_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_ALLOCATE_VIDEO_MEMORY, "ALLOCATE_VIDEO_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_ALLOCATE_LINEAR_VIDEO_MEMORY, "ALLOCATE_LINEAR_VIDEO_MEMORY", hook_AllocateLinearVideoMemory_pre, hook_AllocateLinearVideoMemory_post},
	{gcvHAL_FREE_VIDEO_MEMORY, "FREE_VIDEO_MEMORY", hook_FreeVideoMemory_pre, hook_FreeVideoMemory_post},
	{gcvHAL_MAP_MEMORY, "MAP_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_UNMAP_MEMORY, "UNMAP_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_MAP_USER_MEMORY, "MAP_USER_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_UNMAP_USER_MEMORY, "UNMAP_USER_MEMORY", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_LOCK_VIDEO_MEMORY, "LOCK_VIDEO_MEMORY", hook_LockVideoMemory_pre, hook_LockVideoMemory_post},
	{gcvHAL_UNLOCK_VIDEO_MEMORY, "UNLOCK_VIDEO_MEMORY", hook_UnlockVideoMemory_pre, hook_UnlockVideoMemory_post},
	{gcvHAL_EVENT_COMMIT, "EVENT_COMMIT", hook_EventCommit_pre, hook_EventCommit_post},
	{gcvHAL_USER_SIGNAL, "USER_SIGNAL", hook_UserSignal_pre, hook_UserSignal_post},
	{gcvHAL_SIGNAL, "SIGNAL", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_WRITE_DATA, "WRITE_DATA", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_COMMIT, "COMMIT", hook_Commit_pre, hook_Commit_post},
	{gcvHAL_STALL, "STALL", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_READ_REGISTER, "READ_REGISTER", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_WRITE_REGISTER, "WRITE_REGISTER", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_GET_PROFILE_SETTING, "GET_PROFILE_SETTING", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_SET_PROFILE_SETTING, "SET_PROFILE_SETTING", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_READ_ALL_PROFILE_REGISTERS, "READ_ALL_PROFILE_REGISTERS", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_PROFILE_REGISTERS_2D, "PROFILE_REGISTERS_2D", hook_unknown_pre, hook_unknown_post},
#if VIVANTE_PROFILER_PERDRAW
	{gcvHAL_READ_PROFILER_REGISTER_SETTING, "READ_PROFILER_REGISTER_SETTING", hook_unknown_pre, hook_unknown_post},
#endif
	{gcvHAL_SET_POWER_MANAGEMENT_STATE, "SET_POWER_MANAGEMENT_STATE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_QUERY_POWER_MANAGEMENT_STATE, "QUERY_POWER_MANAGEMENT_STATE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_GET_BASE_ADDRESS, "GET_BASE_ADDRESS", hook_empty_pre, hook_GetBaseAddress_post},
	{gcvHAL_SET_IDLE, "SET_IDLE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_QUERY_KERNEL_SETTINGS, "QUERY_KERNEL_SETTINGS", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_RESET, "RESET", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_MAP_PHYSICAL, "MAP_PHYSICAL", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_DEBUG, "DEBUG", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_CACHE, "CACHE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_TIMESTAMP, "TIMESTAMP", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_DATABASE, "DATABASE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_VERSION, "VERSION", hook_empty_pre, hook_Version_post},
	{gcvHAL_CHIP_INFO, "CHIP_INFO", hook_empty_pre, hook_ChipInfo_post},
	{gcvHAL_ATTACH, "ATTACH", hook_empty_pre, hook_Attach_post},
	{gcvHAL_DETACH, "DETACH", hook_Detach_pre, hook_Detach_post},
	{gcvHAL_COMPOSE, "COMPOSE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_SET_TIMEOUT, "SET_TIMEOUT", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_GET_FRAME_INFO, "GET_FRAME_INFO", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_GET_SHARED_INFO, "GET_SHARED_INFO", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_SET_SHARED_INFO, "SET_SHARED_INFO", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_QUERY_COMMAND_BUFFER, "QUERY_COMMAND_BUFFER", hook_empty_pre, hook_QueryCommandBuffer_post},
	{gcvHAL_COMMIT_DONE, "COMMIT_DONE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_DUMP_GPU_STATE, "DUMP_GPU_STATE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_DUMP_EVENT, "DUMP_EVENT", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_ALLOCATE_VIRTUAL_COMMAND_BUFFER, "ALLOCATE_VIRTUAL_COMMAND_BUFFER", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_FREE_VIRTUAL_COMMAND_BUFFER, "FREE_VIRTUAL_COMMAND_BUFFER", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_SET_FSCALE_VALUE, "SET_FSCALE_VALUE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_GET_FSCALE_VALUE, "GET_FSCALE_VALUE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_QUERY_RESET_TIME_STAMP, "QUERY_RESET_TIME_STAMP", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_SYNC_POINT, "SYNC_POINT", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_CREATE_NATIVE_FENCE, "CREATE_NATIVE_FENCE", hook_unknown_pre, hook_unknown_post},
	{gcvHAL_VIDMEM_DATABASE, "VIDMEM_DATABASE", hook_unknown_pre, hook_unknown_post},
};

static int
galcore_ioctl(int request, void *data)
{
	DRIVER_ARGS *args = data;
	gcsHAL_INTERFACE *input, *output;
	const char *command_name, *hardware;
	int ret, hook_ret;

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
		return -1;
	}

	hardware = viv_hardware_type(input->hardwareType);
	if (!hardware) {
		fprintf(stderr, "%s: unknown hardware type %d\n",
			command_name, input->hardwareType);
		return -1;
	}

	hook_ret = command_table[input->command].pre(command_name, input->hardwareType, (void *) &input->u);
	if (hook_ret) {
		fprintf(stderr, "pre hook for %s(%s) failed.\n", command_name, hardware);
		return -1;
	}

	ret = orig_ioctl(dev_galcore_fd, request, data);

	hook_ret = command_table[input->command].post(command_name, input->hardwareType, (void *) &output->u, ret);
	if (hook_ret) {
		fprintf(stderr, "post hook for %s(%s) failed.\n", command_name, hardware);
		return -1;
	}

	return ret;
}
