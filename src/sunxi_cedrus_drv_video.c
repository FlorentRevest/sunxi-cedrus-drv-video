/*
 * Copyright (c) 2016 Florent Revest, <florent.revest@free-electrons.com>
 *               2007 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <va/va_backend.h>

#include "sunxi_cedrus_drv_video.h"
#include "tiled_yuv.h"

#include "assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <X11/Xlib.h>

#include <linux/videodev2.h>

#define INIT_DRIVER_DATA	struct sunxi_cedrus_driver_data * const driver_data = (struct sunxi_cedrus_driver_data *) ctx->pDriverData;
#define CONFIG(id)  ((object_config_p) object_heap_lookup(&driver_data->config_heap, id))
#define CONTEXT(id) ((object_context_p) object_heap_lookup(&driver_data->context_heap, id))
#define SURFACE(id) ((object_surface_p) object_heap_lookup(&driver_data->surface_heap, id))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup(&driver_data->buffer_heap, id))
#define IMAGE(id)   ((object_image_p) object_heap_lookup(&driver_data->image_heap, id))

#define CONFIG_ID_OFFSET		0x01000000
#define CONTEXT_ID_OFFSET		0x02000000
#define SURFACE_ID_OFFSET		0x04000000
#define BUFFER_ID_OFFSET		0x08000000
#define IMAGE_ID_OFFSET			0x10000000

/* We can't dynamically call VIDIOC_REQBUFS for every MPEG slice we create.
 * Indeed, the queue might be busy processing a previous buffer, so we need to 
 * pre-allocate a set of buffers with a max size */
#define INPUT_BUFFER_MAX_SIZE		131072
#define INPUT_BUFFERS_NUMBER		4

static void sunxi_cedrus_msg(const char *msg, ...)
{
	va_list args;

	fprintf(stderr, "sunxi_cedrus_drv_video: ");
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
}

VAStatus sunxi_cedrus_QueryConfigProfiles(VADriverContextP ctx,
		VAProfile *profile_list, int *num_profiles)
{
	int i = 0;

	profile_list[i++] = VAProfileMPEG2Simple;
	profile_list[i++] = VAProfileMPEG2Main;
	profile_list[i++] = VAProfileMPEG4Simple;
	profile_list[i++] = VAProfileMPEG4AdvancedSimple;
	profile_list[i++] = VAProfileMPEG4Main;
	profile_list[i++] = VAProfileH264Baseline;
	profile_list[i++] = VAProfileH264Main;
	profile_list[i++] = VAProfileH264High;

	assert(i <= SUNXI_CEDRUS_MAX_PROFILES);
	*num_profiles = i;

	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_QueryConfigEntrypoints(VADriverContextP ctx,
		VAProfile profile, VAEntrypoint  *entrypoint_list,
		int *num_entrypoints)
{
	switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			*num_entrypoints = 2;
			entrypoint_list[0] = VAEntrypointVLD;
			entrypoint_list[1] = VAEntrypointMoComp;
			break;

		case VAProfileMPEG4Simple:
		case VAProfileMPEG4AdvancedSimple:
		case VAProfileMPEG4Main:
			*num_entrypoints = 1;
			entrypoint_list[0] = VAEntrypointVLD;
			break;

		case VAProfileH264Baseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			*num_entrypoints = 1;
			entrypoint_list[0] = VAEntrypointVLD;
			break;

		default:
			*num_entrypoints = 0;
			break;
	}

	assert(*num_entrypoints <= SUNXI_CEDRUS_MAX_ENTRYPOINTS);
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_GetConfigAttributes(VADriverContextP ctx,
		VAProfile profile, VAEntrypoint entrypoint,
		VAConfigAttrib *attrib_list, int num_attribs)
{
	int i;

	for (i = 0; i < num_attribs; i++)
	{
		switch (attrib_list[i].type)
		{
			case VAConfigAttribRTFormat:
				attrib_list[i].value = VA_RT_FORMAT_YUV420;
				break;

			default:
				/* Do nothing */
				attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
				break;
		}
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus sunxi_cedrus_update_attribute(object_config_p obj_config, VAConfigAttrib *attrib)
{
	int i;
	/* Check existing attributes */
	for(i = 0; obj_config->attrib_count < i; i++)
	{
		if (obj_config->attrib_list[i].type == attrib->type)
		{
			/* Update existing attribute */
			obj_config->attrib_list[i].value = attrib->value;
			return VA_STATUS_SUCCESS;
		}
	}
	if (obj_config->attrib_count < SUNXI_CEDRUS_MAX_CONFIG_ATTRIBUTES)
	{
		i = obj_config->attrib_count;
		obj_config->attrib_list[i].type = attrib->type;
		obj_config->attrib_list[i].value = attrib->value;
		obj_config->attrib_count++;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus sunxi_cedrus_CreateConfig(VADriverContextP ctx, VAProfile profile,
		VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
		int num_attribs, VAConfigID *config_id)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus;
	int configID;
	object_config_p obj_config;
	int i;

	/* Validate profile & entrypoint */
	switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			if ((VAEntrypointVLD == entrypoint) ||
					(VAEntrypointMoComp == entrypoint))
				vaStatus = VA_STATUS_SUCCESS;
			else
				vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
			break;

		case VAProfileMPEG4Simple:
		case VAProfileMPEG4AdvancedSimple:
		case VAProfileMPEG4Main:
			if (VAEntrypointVLD == entrypoint)
				vaStatus = VA_STATUS_SUCCESS;
			else
				vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
			break;

		case VAProfileH264Baseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			if (VAEntrypointVLD == entrypoint)
				vaStatus = VA_STATUS_SUCCESS;
			else
				vaStatus = VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
			break;

		default:
			vaStatus = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
			break;
	}

	if (VA_STATUS_SUCCESS != vaStatus)
		return vaStatus;

	configID = object_heap_allocate(&driver_data->config_heap);
	obj_config = CONFIG(configID);
	if (NULL == obj_config)
	{
		vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
		return vaStatus;
	}

	obj_config->profile = profile;
	obj_config->entrypoint = entrypoint;
	obj_config->attrib_list[0].type = VAConfigAttribRTFormat;
	obj_config->attrib_list[0].value = VA_RT_FORMAT_YUV420;
	obj_config->attrib_count = 1;

	for(i = 0; i < num_attribs; i++)
	{
		vaStatus = sunxi_cedrus_update_attribute(obj_config, &(attrib_list[i]));
		if (VA_STATUS_SUCCESS != vaStatus)
			break;
	}

	/* Error recovery */
	if (VA_STATUS_SUCCESS != vaStatus)
		object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
	else
		*config_id = configID;

	return vaStatus;
}

VAStatus sunxi_cedrus_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus;
	object_config_p obj_config;

	obj_config = CONFIG(config_id);
	if (NULL == obj_config)
	{
		vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
		return vaStatus;
	}

	object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_QueryConfigAttributes(VADriverContextP ctx, 
		VAConfigID config_id, VAProfile *profile,
		VAEntrypoint *entrypoint, VAConfigAttrib *attrib_list,
		int *num_attribs)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_config_p obj_config;
	int i;

	obj_config = CONFIG(config_id);
	assert(obj_config);

	*profile = obj_config->profile;
	*entrypoint = obj_config->entrypoint;
	*num_attribs =  obj_config->attrib_count;
	for(i = 0; i < obj_config->attrib_count; i++)
		attrib_list[i] = obj_config->attrib_list[i];

	return vaStatus;
}

VAStatus sunxi_cedrus_CreateSurfaces(VADriverContextP ctx, int width, 
		int height, int format, int num_surfaces, VASurfaceID *surfaces)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	int i;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];
	struct v4l2_create_buffers create_bufs;
	struct v4l2_format fmt;

	/* We only support one format */
	if (VA_RT_FORMAT_YUV420 != format)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/* Set format for capture */
	memset(&(fmt), 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
	fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.num_planes = 2;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_S_FMT, &fmt)==0);

	memset (&create_bufs, 0, sizeof (struct v4l2_create_buffers));
	create_bufs.count = num_surfaces;
	create_bufs.memory = V4L2_MEMORY_MMAP;
	create_bufs.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_G_FMT, &create_bufs.format)==0);
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_CREATE_BUFS, &create_bufs)==0);
	driver_data->num_dst_bufs = create_bufs.count;

	for (i = 0; i < create_bufs.count; i++)
	{
		int surfaceID = object_heap_allocate(&driver_data->surface_heap);
		object_surface_p obj_surface = SURFACE(surfaceID);
		if (NULL == obj_surface)
		{
			vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
			break;
		}
		obj_surface->surface_id = surfaceID;
		surfaces[i] = surfaceID;

		memset(&(buf), 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = create_bufs.index + i;
		buf.length = 2;
		buf.m.planes = planes;

		assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QUERYBUF, &buf)==0);

		driver_data->luma_bufs[buf.index] = mmap(NULL, buf.m.planes[0].length,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				driver_data->mem2mem_fd, buf.m.planes[0].m.mem_offset);
		assert(driver_data->luma_bufs[buf.index] != MAP_FAILED);

		driver_data->chroma_bufs[buf.index] = mmap(NULL, buf.m.planes[1].length,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				driver_data->mem2mem_fd, buf.m.planes[1].m.mem_offset);
		assert(driver_data->chroma_bufs[buf.index] != MAP_FAILED);

		obj_surface->input_buf_index = 0;
		obj_surface->output_buf_index = 0;

		obj_surface->width = width;
		obj_surface->height = height;
		obj_surface->status = VASurfaceReady;

		assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QBUF, &buf)==0);
	}

	/* Error recovery */
	if (VA_STATUS_SUCCESS != vaStatus)
	{
		/* surfaces[i-1] was the last successful allocation */
		for(; i--;)
		{
			object_surface_p obj_surface = SURFACE(surfaces[i]);
			surfaces[i] = VA_INVALID_SURFACE;
			assert(obj_surface);
			object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
		}
	}

	return vaStatus;
}

VAStatus sunxi_cedrus_DestroySurfaces(VADriverContextP ctx, 
		VASurfaceID *surface_list, int num_surfaces)
{
	INIT_DRIVER_DATA
	int i;
	for(i = num_surfaces; i--;)
	{
		object_surface_p obj_surface = SURFACE(surface_list[i]);
		assert(obj_surface);
		object_heap_free(&driver_data->surface_heap, (object_base_p) obj_surface);
	}
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_CreateContext(VADriverContextP ctx, VAConfigID config_id,
		int picture_width, int picture_height, int flag,
		VASurfaceID *render_targets, int num_render_targets,
		VAContextID *context)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_config_p obj_config;
	int i;
	struct v4l2_create_buffers create_bufs;
	struct v4l2_format fmt;

	obj_config = CONFIG(config_id);
	if (NULL == obj_config)
	{
		vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
		return vaStatus;
	}

	/* Validate flag */
	/* Validate picture dimensions */

	int contextID = object_heap_allocate(&driver_data->context_heap);
	object_context_p obj_context = CONTEXT(contextID);
	if (NULL == obj_context)
	{
		vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
		return vaStatus;
	}

	obj_context->context_id  = contextID;
	*context = contextID;
	obj_context->current_render_target = -1;
	obj_context->config_id = config_id;
	obj_context->picture_width = picture_width;
	obj_context->picture_height = picture_height;
	obj_context->num_render_targets = num_render_targets;
	obj_context->render_targets = (VASurfaceID *) malloc(num_render_targets * sizeof(VASurfaceID));
	obj_context->num_rendered_surfaces = 0;

	if (obj_context->render_targets == NULL)
	{
		vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
		return vaStatus;
	}

	for(i = 0; i < num_render_targets; i++)
	{
		if (NULL == SURFACE(render_targets[i]))
		{
			vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
			break;
		}
		obj_context->render_targets[i] = render_targets[i];
	}
	obj_context->flags = flag;

	/* Error recovery */
	if (VA_STATUS_SUCCESS != vaStatus)
	{
		obj_context->context_id = -1;
		obj_context->config_id = -1;
		free(obj_context->render_targets);
		obj_context->render_targets = NULL;
		obj_context->num_render_targets = 0;
		obj_context->flags = 0;
		object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);
	}

	memset(&(fmt), 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = picture_width;
	fmt.fmt.pix_mp.height = picture_height;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = INPUT_BUFFER_MAX_SIZE;
	switch(obj_config->profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MPEG2_FRAME;
			break;
		case VAProfileMPEG4Simple:
		case VAProfileMPEG4AdvancedSimple:
		case VAProfileMPEG4Main:
			fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MPEG4_FRAME;
			break;
		case VAProfileH264Baseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_FRAME;
			break;
		default:
			vaStatus = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
			return vaStatus;
			break;
	}
	fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	fmt.fmt.pix_mp.num_planes = 1;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_S_FMT, &fmt)==0);

	memset (&create_bufs, 0, sizeof (struct v4l2_create_buffers));
	create_bufs.count = INPUT_BUFFERS_NUMBER;
	create_bufs.memory = V4L2_MEMORY_MMAP;
	create_bufs.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_G_FMT, &create_bufs.format)==0);
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_CREATE_BUFS, &create_bufs)==0);

	return vaStatus;
}

VAStatus sunxi_cedrus_DestroyContext(VADriverContextP ctx, VAContextID context)
{
	INIT_DRIVER_DATA
	object_context_p obj_context = CONTEXT(context);
	assert(obj_context);

	obj_context->context_id = -1;
	obj_context->config_id = -1;
	obj_context->picture_width = 0;
	obj_context->picture_height = 0;
	if (obj_context->render_targets)
		free(obj_context->render_targets);
	obj_context->render_targets = NULL;
	obj_context->num_render_targets = 0;
	obj_context->flags = 0;

	obj_context->current_render_target = -1;

	object_heap_free(&driver_data->context_heap, (object_base_p) obj_context);

	return VA_STATUS_SUCCESS;
}

static VAStatus sunxi_cedrus_allocate_buffer(VADriverContextP ctx, VAContextID context,
		object_buffer_p obj_buffer, int size)
{
	INIT_DRIVER_DATA
	struct v4l2_plane plane[1];

	if(obj_buffer->type == VASliceDataBufferType) {
		object_context_p obj_context;

		obj_context = CONTEXT(context);
		assert(obj_context);

		struct v4l2_buffer buf;
		memset(&(buf), 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = obj_context->num_rendered_surfaces%INPUT_BUFFERS_NUMBER;
		buf.length = 1;
		buf.m.planes = plane;

		assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QUERYBUF, &buf)==0);

		obj_buffer->buffer_data = mmap(NULL, size,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				driver_data->mem2mem_fd, buf.m.planes[0].m.mem_offset);
	} else
		obj_buffer->buffer_data = realloc(obj_buffer->buffer_data, size);

	if (obj_buffer->buffer_data == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_CreateBuffer(VADriverContextP ctx, VAContextID context,
		VABufferType type, unsigned int size, unsigned int num_elements,
		void *data, VABufferID *buf_id)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	int bufferID;
	object_buffer_p obj_buffer;

	/* Validate type */
	switch (type)
	{
		case VAPictureParameterBufferType:
		case VAIQMatrixBufferType: /* Ignored */
		case VASliceParameterBufferType:
		case VASliceDataBufferType:
		case VAImageBufferType:
			/* Ok */
			break;
		default:
			vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
			return vaStatus;
	}

	bufferID = object_heap_allocate(&driver_data->buffer_heap);
	obj_buffer = BUFFER(bufferID);
	if (NULL == obj_buffer)
	{
		vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
		return vaStatus;
	}

	obj_buffer->buffer_data = NULL;
	obj_buffer->type = type;

	vaStatus = sunxi_cedrus_allocate_buffer(ctx, context, obj_buffer, size * num_elements);
	if (VA_STATUS_SUCCESS == vaStatus)
	{
		obj_buffer->max_num_elements = num_elements;
		obj_buffer->num_elements = num_elements;
		obj_buffer->size = size;

		if (data)
			memcpy(obj_buffer->buffer_data, data, size * num_elements);
	}

	if (VA_STATUS_SUCCESS == vaStatus)
		*buf_id = bufferID;

	return vaStatus;
}

VAStatus sunxi_cedrus_BufferSetNumElements(VADriverContextP ctx,
	       	VABufferID buf_id, unsigned int num_elements)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_buffer_p obj_buffer = BUFFER(buf_id);
	assert(obj_buffer);

	if ((num_elements < 0) || (num_elements > obj_buffer->max_num_elements))
		vaStatus = VA_STATUS_ERROR_UNKNOWN;
	if (VA_STATUS_SUCCESS == vaStatus)
		obj_buffer->num_elements = num_elements;

	return vaStatus;
}

VAStatus sunxi_cedrus_MapBuffer(VADriverContextP ctx, VABufferID buf_id,
		void **pbuf)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_ERROR_UNKNOWN;
	object_buffer_p obj_buffer = BUFFER(buf_id);
	assert(obj_buffer);
	if (NULL == obj_buffer)
	{
		vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
		return vaStatus;
	}

	if (NULL != obj_buffer->buffer_data)
	{
		*pbuf = obj_buffer->buffer_data;
		vaStatus = VA_STATUS_SUCCESS;
	}
	return vaStatus;
}

VAStatus sunxi_cedrus_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	/* Do nothing */
	return VA_STATUS_SUCCESS;
}

static void sunxi_cedrus_destroy_buffer(struct sunxi_cedrus_driver_data *driver_data, object_buffer_p obj_buffer)
{
	if (NULL != obj_buffer->buffer_data)
	{
		if(obj_buffer->type == VASliceDataBufferType)
			munmap(obj_buffer->buffer_data, obj_buffer->size);
		else
			free(obj_buffer->buffer_data);

		obj_buffer->buffer_data = NULL;
	}

	object_heap_free(&driver_data->buffer_heap, (object_base_p) obj_buffer);
}

VAStatus sunxi_cedrus_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
	INIT_DRIVER_DATA
	object_buffer_p obj_buffer = BUFFER(buffer_id);
	assert(obj_buffer);

	sunxi_cedrus_destroy_buffer(driver_data, obj_buffer);
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_BeginPicture(VADriverContextP ctx, VAContextID context,
		VASurfaceID render_target)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_context_p obj_context;
	object_surface_p obj_surface;

	obj_context = CONTEXT(context);
	assert(obj_context);

	obj_surface = SURFACE(render_target);
	assert(obj_surface);

	if(obj_surface->status == VASurfaceRendering)
		sunxi_cedrus_SyncSurface(ctx, render_target);

	obj_surface->status = VASurfaceRendering;
	obj_surface->request = (obj_context->num_rendered_surfaces)%INPUT_BUFFERS_NUMBER+1;
	obj_surface->input_buf_index = obj_context->num_rendered_surfaces%INPUT_BUFFERS_NUMBER;
	obj_surface->output_buf_index = obj_context->num_rendered_surfaces%driver_data->num_dst_bufs;
	obj_context->num_rendered_surfaces ++;

	obj_context->current_render_target = obj_surface->base.id;

	return vaStatus;
}

VAStatus sunxi_cedrus_render_mpeg2_slice_data(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[1];

	/* Query */
	memset(&(buf), 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->input_buf_index;
	buf.length = 1;
	buf.m.planes = plane;

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QUERYBUF, &buf)==0);

	/* Populate frame */
	char *src_buf = mmap(NULL, obj_buffer->size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			driver_data->mem2mem_fd, buf.m.planes[0].m.mem_offset);
	assert(src_buf != MAP_FAILED);
	memcpy(src_buf, obj_buffer->buffer_data, obj_buffer->size);

	memset(&(buf), 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->input_buf_index;
	buf.length = 1;
	buf.m.planes = plane;
	buf.m.planes[0].bytesused = obj_buffer->size;
	buf.request = obj_surface->request;

	obj_context->mpeg2_frame_hdr.slice_pos = 0;
	obj_context->mpeg2_frame_hdr.slice_len = obj_buffer->size;
	obj_context->mpeg2_frame_hdr.type = MPEG2;

	struct v4l2_ext_control ctrl;
	struct v4l2_ext_controls extCtrls;

	ctrl.id = V4L2_CID_MPEG_VIDEO_MPEG2_FRAME_HDR;
	ctrl.ptr = &obj_context->mpeg2_frame_hdr;
	ctrl.size = sizeof(obj_context->mpeg2_frame_hdr);

	extCtrls.controls = &ctrl;
	extCtrls.count = 1;
	extCtrls.request = obj_surface->request;

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_S_EXT_CTRLS, &extCtrls)==0);

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QBUF, &buf)==0);

	return vaStatus;
}

VAStatus sunxi_cedrus_render_mpeg2_picture_parameter(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;

	VAPictureParameterBufferMPEG2 *pic_param = (VAPictureParameterBufferMPEG2 *)obj_buffer->buffer_data;

	obj_context->mpeg2_frame_hdr.width = pic_param->horizontal_size;
	obj_context->mpeg2_frame_hdr.height = pic_param->vertical_size;

	obj_context->mpeg2_frame_hdr.picture_coding_type = pic_param->picture_coding_type;
	obj_context->mpeg2_frame_hdr.f_code[0][0] = (pic_param->f_code >> 12) & 0xf;
	obj_context->mpeg2_frame_hdr.f_code[0][1] = (pic_param->f_code >>  8) & 0xf;
	obj_context->mpeg2_frame_hdr.f_code[1][0] = (pic_param->f_code >>  4) & 0xf;
	obj_context->mpeg2_frame_hdr.f_code[1][1] = pic_param->f_code & 0xf;

	obj_context->mpeg2_frame_hdr.intra_dc_precision = pic_param->picture_coding_extension.bits.intra_dc_precision;
	obj_context->mpeg2_frame_hdr.picture_structure = pic_param->picture_coding_extension.bits.picture_structure;
	obj_context->mpeg2_frame_hdr.top_field_first = pic_param->picture_coding_extension.bits.top_field_first;
	obj_context->mpeg2_frame_hdr.frame_pred_frame_dct = pic_param->picture_coding_extension.bits.frame_pred_frame_dct;
	obj_context->mpeg2_frame_hdr.concealment_motion_vectors = pic_param->picture_coding_extension.bits.concealment_motion_vectors;
	obj_context->mpeg2_frame_hdr.q_scale_type = pic_param->picture_coding_extension.bits.q_scale_type;
	obj_context->mpeg2_frame_hdr.intra_vlc_format = pic_param->picture_coding_extension.bits.intra_vlc_format;
	obj_context->mpeg2_frame_hdr.alternate_scan = pic_param->picture_coding_extension.bits.alternate_scan;

	object_surface_p fwd_surface = SURFACE(pic_param->forward_reference_picture);
	if(fwd_surface)
		obj_context->mpeg2_frame_hdr.forward_index = fwd_surface->output_buf_index;
	else
		obj_context->mpeg2_frame_hdr.forward_index = obj_surface->output_buf_index;
	object_surface_p bwd_surface = SURFACE(pic_param->backward_reference_picture);
	if(bwd_surface)
		obj_context->mpeg2_frame_hdr.backward_index = bwd_surface->output_buf_index;
	else
		obj_context->mpeg2_frame_hdr.backward_index = obj_surface->output_buf_index;

	return vaStatus;
}

VAStatus sunxi_cedrus_render_mpeg4_slice_data(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[1];

	/* Query */
	memset(&(buf), 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->input_buf_index;
	buf.length = 1;
	buf.m.planes = plane;

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QUERYBUF, &buf)==0);

	/* Populate frame */
	char *src_buf = mmap(NULL, obj_buffer->size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			driver_data->mem2mem_fd, buf.m.planes[0].m.mem_offset);
	assert(src_buf != MAP_FAILED);
	memcpy(src_buf, obj_buffer->buffer_data, obj_buffer->size);

	memset(&(buf), 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->input_buf_index;
	buf.length = 1;
	buf.m.planes = plane;
	buf.m.planes[0].bytesused = obj_buffer->size;
	buf.request = obj_surface->request;

	struct v4l2_ext_control ctrl;
	struct v4l2_ext_controls extCtrls;

	ctrl.id = V4L2_CID_MPEG_VIDEO_MPEG4_FRAME_HDR;
	ctrl.ptr = &obj_context->mpeg4_frame_hdr;
	ctrl.size = sizeof(obj_context->mpeg4_frame_hdr);

	extCtrls.controls = &ctrl;
	extCtrls.count = 1;
	extCtrls.request = obj_surface->request;

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_S_EXT_CTRLS, &extCtrls)==0);

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QBUF, &buf)==0);

	return vaStatus;
}

VAStatus sunxi_cedrus_render_mpeg4_picture_parameter(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;

	VAPictureParameterBufferMPEG4 *pic_param = (VAPictureParameterBufferMPEG4 *)obj_buffer->buffer_data;

	obj_context->mpeg4_frame_hdr.width = pic_param->vop_width;
	obj_context->mpeg4_frame_hdr.height = pic_param->vop_height;

	obj_context->mpeg4_frame_hdr.vol_fields.short_video_header = pic_param->vol_fields.bits.short_video_header;
	obj_context->mpeg4_frame_hdr.vol_fields.chroma_format = pic_param->vol_fields.bits.chroma_format;
	obj_context->mpeg4_frame_hdr.vol_fields.interlaced = pic_param->vol_fields.bits.interlaced;
	obj_context->mpeg4_frame_hdr.vol_fields.obmc_disable = pic_param->vol_fields.bits.obmc_disable;
	obj_context->mpeg4_frame_hdr.vol_fields.sprite_enable = pic_param->vol_fields.bits.sprite_enable;
	obj_context->mpeg4_frame_hdr.vol_fields.sprite_warping_accuracy = pic_param->vol_fields.bits.sprite_warping_accuracy;
	obj_context->mpeg4_frame_hdr.vol_fields.quant_type = pic_param->vol_fields.bits.quant_type;
	obj_context->mpeg4_frame_hdr.vol_fields.quarter_sample = pic_param->vol_fields.bits.quarter_sample;
	obj_context->mpeg4_frame_hdr.vol_fields.data_partitioned = pic_param->vol_fields.bits.data_partitioned;
	obj_context->mpeg4_frame_hdr.vol_fields.reversible_vlc = pic_param->vol_fields.bits.reversible_vlc;
	obj_context->mpeg4_frame_hdr.vol_fields.resync_marker_disable = pic_param->vol_fields.bits.resync_marker_disable;

	obj_context->mpeg4_frame_hdr.quant_precision = pic_param->quant_precision;

	obj_context->mpeg4_frame_hdr.vop_fields.vop_coding_type = pic_param->vop_fields.bits.vop_coding_type;
	obj_context->mpeg4_frame_hdr.vop_fields.backward_reference_vop_coding_type = pic_param->vop_fields.bits.backward_reference_vop_coding_type;
	obj_context->mpeg4_frame_hdr.vop_fields.vop_rounding_type = pic_param->vop_fields.bits.vop_rounding_type;
	obj_context->mpeg4_frame_hdr.vop_fields.intra_dc_vlc_thr = pic_param->vop_fields.bits.intra_dc_vlc_thr;
	obj_context->mpeg4_frame_hdr.vop_fields.top_field_first = pic_param->vop_fields.bits.top_field_first;
	obj_context->mpeg4_frame_hdr.vop_fields.alternate_vertical_scan_flag = pic_param->vop_fields.bits.alternate_vertical_scan_flag;

	obj_context->mpeg4_frame_hdr.vop_fcode_forward = pic_param->vop_fcode_forward;
	obj_context->mpeg4_frame_hdr.vop_fcode_backward = pic_param->vop_fcode_backward;

	obj_context->mpeg4_frame_hdr.trb = pic_param->TRB;
	obj_context->mpeg4_frame_hdr.trd = pic_param->TRD;

	object_surface_p fwd_surface = SURFACE(pic_param->forward_reference_picture);
	if(fwd_surface)
		obj_context->mpeg4_frame_hdr.forward_index = fwd_surface->output_buf_index;
	else
		obj_context->mpeg4_frame_hdr.forward_index = obj_surface->output_buf_index;
	object_surface_p bwd_surface = SURFACE(pic_param->backward_reference_picture);
	if(bwd_surface)
		obj_context->mpeg4_frame_hdr.backward_index = bwd_surface->output_buf_index;
	else
		obj_context->mpeg4_frame_hdr.backward_index = obj_surface->output_buf_index;

	return vaStatus;
}

VAStatus sunxi_cedrus_render_mpeg4_slice_parameter(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	VASliceParameterBufferMPEG4 *slice_param = (VASliceParameterBufferMPEG4 *)obj_buffer->buffer_data;

	obj_context->mpeg4_frame_hdr.slice_pos = slice_param->slice_data_offset;
	obj_context->mpeg4_frame_hdr.slice_len = slice_param->slice_data_size;

	return VA_STATUS_SUCCESS;
}


VAStatus sunxi_cedrus_render_h264_slice_data(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_render_h264_picture_parameter(VADriverContextP ctx, object_context_p obj_context, 
		object_surface_p obj_surface, object_buffer_p obj_buffer)
{
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_RenderPicture(VADriverContextP ctx, VAContextID context,
		VABufferID *buffers, int num_buffers)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_context_p obj_context;
	object_surface_p obj_surface;
	object_config_p obj_config;
	int i;

	obj_context = CONTEXT(context);
	assert(obj_context);

	obj_config = CONFIG(obj_context->config_id);
	if (NULL == obj_config)
	{
		vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
		return vaStatus;
	}

	obj_surface = SURFACE(obj_context->current_render_target);
	assert(obj_surface);

	/* verify that we got valid buffer references */
	for(i = 0; i < num_buffers; i++)
	{
		object_buffer_p obj_buffer = BUFFER(buffers[i]);
		assert(obj_buffer);
		if (NULL == obj_buffer)
		{
			vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
			break;
		}

		switch(obj_config->profile) {
			case VAProfileMPEG2Simple:
			case VAProfileMPEG2Main:
				if(obj_buffer->type == VASliceDataBufferType)
					vaStatus = sunxi_cedrus_render_mpeg2_slice_data(ctx, obj_context, obj_surface, obj_buffer);
				else if(obj_buffer->type == VAPictureParameterBufferType)
					vaStatus = sunxi_cedrus_render_mpeg2_picture_parameter(ctx, obj_context, obj_surface, obj_buffer);
				break;
			case VAProfileMPEG4Simple:
			case VAProfileMPEG4AdvancedSimple:
			case VAProfileMPEG4Main:
				if(obj_buffer->type == VASliceDataBufferType)
					vaStatus = sunxi_cedrus_render_mpeg4_slice_data(ctx, obj_context, obj_surface, obj_buffer);
				else if(obj_buffer->type == VAPictureParameterBufferType)
					vaStatus = sunxi_cedrus_render_mpeg4_picture_parameter(ctx, obj_context, obj_surface, obj_buffer);
				else if(obj_buffer->type == VASliceParameterBufferType)
					vaStatus = sunxi_cedrus_render_mpeg4_slice_parameter(ctx, obj_context, obj_surface, obj_buffer);
				break;
			case VAProfileH264Baseline:
			case VAProfileH264Main:
			case VAProfileH264High:
				if(obj_buffer->type == VASliceDataBufferType)
					vaStatus = sunxi_cedrus_render_h264_slice_data(ctx, obj_context, obj_surface, obj_buffer);
				else if(obj_buffer->type == VAPictureParameterBufferType)
					vaStatus = sunxi_cedrus_render_h264_picture_parameter(ctx, obj_context, obj_surface, obj_buffer);
			default:
				break;
		}
	}

	return vaStatus;
}

VAStatus sunxi_cedrus_EndPicture(VADriverContextP ctx, VAContextID context)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_context_p obj_context;
	object_surface_p obj_surface;
	enum v4l2_buf_type type;

	obj_context = CONTEXT(context);
	assert(obj_context);

	obj_surface = SURFACE(obj_context->current_render_target);
	assert(obj_surface);

	/* For now, assume that we are done with rendering right away */
	obj_context->current_render_target = -1;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_STREAMON, &type)==0);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_STREAMON, &type)==0);

	return vaStatus;
}

VAStatus sunxi_cedrus_SyncSurface(VADriverContextP ctx,
		VASurfaceID render_target)
{
	INIT_DRIVER_DATA
	object_surface_p obj_surface;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[1];
        fd_set read_fds;

        FD_ZERO(&read_fds);
        FD_SET(driver_data->mem2mem_fd, &read_fds);
        select(driver_data->mem2mem_fd + 1, &read_fds, NULL, NULL, 0);

	obj_surface = SURFACE(render_target);
	assert(obj_surface);

	memset(&(buf), 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->input_buf_index;
	buf.length = 1;
	buf.m.planes = plane;

	if(ioctl(driver_data->mem2mem_fd, VIDIOC_DQBUF, &buf))
		return VA_STATUS_ERROR_UNKNOWN;

	memset(&(buf), 0, sizeof(buf));
	struct v4l2_plane planes[2];
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = obj_surface->output_buf_index;
	buf.length = 2;
	buf.m.planes = planes;

	obj_surface->status = VASurfaceReady;

	if(ioctl(driver_data->mem2mem_fd, VIDIOC_DQBUF, &buf))
		return VA_STATUS_ERROR_UNKNOWN;

	if(ioctl(driver_data->mem2mem_fd, VIDIOC_QBUF, &buf))
		return VA_STATUS_ERROR_UNKNOWN;

	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_QuerySurfaceStatus(VADriverContextP ctx,
		VASurfaceID render_target, VASurfaceStatus *status)
{
	INIT_DRIVER_DATA
	VAStatus vaStatus = VA_STATUS_SUCCESS;
	object_surface_p obj_surface;

	obj_surface = SURFACE(render_target);
	assert(obj_surface);

	*status = obj_surface->status;

	return vaStatus;
}

/* WARNING: This is for development purpose only!!! */
VAStatus sunxi_cedrus_PutSurface(VADriverContextP ctx, VASurfaceID surface,
		void *draw, short srcx, short srcy, unsigned short srcw,
		unsigned short srch, short destx, short desty, 
		unsigned short destw, unsigned short desth,
		VARectangle *cliprects, unsigned int number_cliprects,
		unsigned int flags)
{
	INIT_DRIVER_DATA
	GC gc;
	Display *display;
	const XID xid = (XID)(uintptr_t)draw;
	XColor xcolor;
	int screen;
	Colormap cm;
	int colorratio = 65535 / 255;
	int x, y;
	object_surface_p obj_surface;

	obj_surface = SURFACE(surface);
	assert(obj_surface);

	display = XOpenDisplay(getenv("DISPLAY"));
	if (display == NULL) {
		sunxi_cedrus_msg("Cannot connect to X server\n");
		exit(1);
	}

	sunxi_cedrus_msg("warning: using vaPutSurface with sunxi-cedrus is not recommended\n");
	screen = DefaultScreen(display);
	gc =  XCreateGC(display, RootWindow(display, screen), 0, NULL);
	XSync(display, False);

	cm = DefaultColormap(display, screen);
	xcolor.flags = DoRed | DoGreen | DoBlue;

	for(x=destx; x < destx+destw; x++) {
		for(y=desty; y < desty+desth; y++) {
			char lum = driver_data->luma_bufs[obj_surface->output_buf_index][x+srcw*y];
			xcolor.red = xcolor.green = xcolor.blue = lum*colorratio;
			XAllocColor(display, cm, &xcolor);
			XSetForeground(display, gc, xcolor.pixel);
			XDrawPoint(display, xid, gc, x, y);
		}
	}

	XFlush(display);
	XCloseDisplay(display);
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_QueryImageFormats(VADriverContextP ctx,
		VAImageFormat *format_list, int *num_formats)
{ 
	format_list[0].fourcc = VA_FOURCC_NV12;
	*num_formats = 1;	
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_CreateImage(VADriverContextP ctx, VAImageFormat *format,
		int width, int height, VAImage *image)
{
	INIT_DRIVER_DATA
	int sizeY, sizeUV;
	object_image_p obj_img;

	image->format = *format;
	image->buf = VA_INVALID_ID;
	image->width = width;
	image->height = height;

	sizeY    = image->width * image->height;
	sizeUV   = ((image->width+1) * (image->height+1)/2);

	image->num_planes = 2;
	image->pitches[0] = (image->width+31)&~31;
	image->pitches[1] = (image->width+31)&~31;
	image->offsets[0] = 0;
	image->offsets[1] = sizeY;
	image->data_size  = sizeY + sizeUV;

	image->image_id = object_heap_allocate(&driver_data->image_heap);
	if (image->image_id == VA_INVALID_ID)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	obj_img = IMAGE(image->image_id);

	if (sunxi_cedrus_CreateBuffer(ctx, 0, VAImageBufferType, image->data_size, 
	    1, NULL, &image->buf) != VA_STATUS_SUCCESS)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	obj_img->buf = image->buf;

	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_DeriveImage(VADriverContextP ctx, VASurfaceID surface,
		VAImage *image)
{
	INIT_DRIVER_DATA
	object_surface_p obj_surface;
	VAImageFormat fmt;
	object_buffer_p obj_buffer;
	VAStatus ret;

	obj_surface = SURFACE(surface);
	fmt.fourcc = VA_FOURCC_NV12;

	ret = sunxi_cedrus_CreateImage(ctx, &fmt, obj_surface->width,
			obj_surface->height, image);
	if(ret != VA_STATUS_SUCCESS)
		return ret;

	obj_buffer = BUFFER(image->buf);
	if (NULL == obj_buffer)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	/* TODO: move to kernel side */
	tiled_to_planar(driver_data->luma_bufs[obj_surface->output_buf_index], obj_buffer->buffer_data, image->pitches[0], image->width, image->height);
	tiled_to_planar(driver_data->chroma_bufs[obj_surface->output_buf_index], obj_buffer->buffer_data + image->width*image->height, image->pitches[1], image->width, image->height/2);

	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_DestroyImage(VADriverContextP ctx, VAImageID image)
{
	INIT_DRIVER_DATA
	object_image_p obj_img;

	obj_img = IMAGE(image);
	assert(obj_img);

	sunxi_cedrus_DestroyBuffer(ctx, obj_img->buf);
	return VA_STATUS_SUCCESS;
}

VAStatus sunxi_cedrus_SetImagePalette(VADriverContextP ctx, VAImageID image,
		unsigned char *palette)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_GetImage(VADriverContextP ctx, VASurfaceID surface,
		int x, int y, unsigned int width, unsigned int height,
		VAImageID image)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_PutImage(VADriverContextP ctx, VASurfaceID surface,
		VAImageID image, int src_x, int src_y, unsigned int src_width,
		unsigned int src_height, int dest_x, int dest_y,
		unsigned int dest_width, unsigned int dest_height)
{ return VA_STATUS_SUCCESS; }

/* sunxi-cedrus doesn't support Subpictures */
VAStatus sunxi_cedrus_QuerySubpictureFormats(VADriverContextP ctx,
		VAImageFormat *format_list, unsigned int *flags,
		unsigned int *num_formats)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_CreateSubpicture(VADriverContextP ctx, VAImageID image,
		VASubpictureID *subpicture)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_DestroySubpicture(VADriverContextP ctx,
		VASubpictureID subpicture)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_SetSubpictureImage(VADriverContextP ctx,
		VASubpictureID subpicture, VAImageID image)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_SetSubpicturePalette(VADriverContextP ctx,
		VASubpictureID subpicture, unsigned char *palette)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_SetSubpictureChromakey(VADriverContextP ctx,
		VASubpictureID subpicture, unsigned int chromakey_min,
		unsigned int chromakey_max, unsigned int chromakey_mask)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_SetSubpictureGlobalAlpha(VADriverContextP ctx,
		VASubpictureID subpicture, float global_alpha)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_AssociateSubpicture(VADriverContextP ctx,
		VASubpictureID subpicture, VASurfaceID *target_surfaces,
		int num_surfaces, short src_x, short src_y,
		unsigned short src_width, unsigned short src_height,
		short dest_x, short dest_y, unsigned short dest_width,
		unsigned short dest_height, unsigned int flags)
{ return VA_STATUS_SUCCESS; }

VAStatus sunxi_cedrus_DeassociateSubpicture(VADriverContextP ctx,
		VASubpictureID subpicture, VASurfaceID *target_surfaces,
		int num_surfaces)
{ return VA_STATUS_SUCCESS; }

/* sunxi-cedrus doesn't support display attributes */
VAStatus sunxi_cedrus_QueryDisplayAttributes (VADriverContextP ctx,
		VADisplayAttribute *attr_list, int *num_attributes)
{ return VA_STATUS_ERROR_UNKNOWN; }

VAStatus sunxi_cedrus_GetDisplayAttributes (VADriverContextP ctx,
		VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_ERROR_UNKNOWN; }

VAStatus sunxi_cedrus_SetDisplayAttributes (VADriverContextP ctx, 
		VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_ERROR_UNKNOWN; }

/* sunxi-cedrus doesn't support buffer info and lock */
VAStatus sunxi_cedrus_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
		VABufferType *type, unsigned int *size,
		unsigned int *num_elements)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus sunxi_cedrus_LockSurface(VADriverContextP ctx, VASurfaceID surface,
		unsigned int *fourcc, unsigned int *luma_stride, 
		unsigned int *chroma_u_stride, unsigned int *chroma_v_stride,
		unsigned int *luma_offset, unsigned int *chroma_u_offset,
		unsigned int *chroma_v_offset, unsigned int *buffer_name,
		void **buffer)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus sunxi_cedrus_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus sunxi_cedrus_Terminate(VADriverContextP ctx)
{
	INIT_DRIVER_DATA
	object_buffer_p obj_buffer;
	object_config_p obj_config;
	object_heap_iterator iter;
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(driver_data->mem2mem_fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(driver_data->mem2mem_fd, VIDIOC_STREAMOFF, &type);

	close(driver_data->mem2mem_fd);

	/* Clean up left over buffers */
	obj_buffer = (object_buffer_p) object_heap_first(&driver_data->buffer_heap, &iter);
	while (obj_buffer)
	{
		sunxi_cedrus_msg("vaTerminate: bufferID %08x still allocated, destroying\n", obj_buffer->base.id);
		sunxi_cedrus_destroy_buffer(driver_data, obj_buffer);
		obj_buffer = (object_buffer_p) object_heap_next(&driver_data->buffer_heap, &iter);
	}

	object_heap_destroy(&driver_data->buffer_heap);
	object_heap_destroy(&driver_data->surface_heap);
	object_heap_destroy(&driver_data->context_heap);

	/* Clean up configIDs */
	obj_config = (object_config_p) object_heap_first(&driver_data->config_heap, &iter);
	while (obj_config)
	{
		object_heap_free(&driver_data->config_heap, (object_base_p) obj_config);
		obj_config = (object_config_p) object_heap_next(&driver_data->config_heap, &iter);
	}
	object_heap_destroy(&driver_data->config_heap);

	free(ctx->pDriverData);
	ctx->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}

VAStatus __attribute__((visibility("default")))
VA_DRIVER_INIT_FUNC(VADriverContextP ctx);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP ctx)
{
	struct VADriverVTable * const vtable = ctx->vtable;
	struct sunxi_cedrus_driver_data *driver_data;
	struct v4l2_capability cap;

	ctx->version_major = VA_MAJOR_VERSION;
	ctx->version_minor = VA_MINOR_VERSION;
	ctx->max_profiles = SUNXI_CEDRUS_MAX_PROFILES;
	ctx->max_entrypoints = SUNXI_CEDRUS_MAX_ENTRYPOINTS;
	ctx->max_attributes = SUNXI_CEDRUS_MAX_CONFIG_ATTRIBUTES;
	ctx->max_image_formats = SUNXI_CEDRUS_MAX_IMAGE_FORMATS;
	ctx->max_subpic_formats = SUNXI_CEDRUS_MAX_SUBPIC_FORMATS;
	ctx->max_display_attributes = SUNXI_CEDRUS_MAX_DISPLAY_ATTRIBUTES;
	ctx->str_vendor = SUNXI_CEDRUS_STR_VENDOR;

	vtable->vaTerminate = sunxi_cedrus_Terminate;
	vtable->vaQueryConfigEntrypoints = sunxi_cedrus_QueryConfigEntrypoints;
	vtable->vaQueryConfigProfiles = sunxi_cedrus_QueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = sunxi_cedrus_QueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = sunxi_cedrus_QueryConfigAttributes;
	vtable->vaCreateConfig = sunxi_cedrus_CreateConfig;
	vtable->vaDestroyConfig = sunxi_cedrus_DestroyConfig;
	vtable->vaGetConfigAttributes = sunxi_cedrus_GetConfigAttributes;
	vtable->vaCreateSurfaces = sunxi_cedrus_CreateSurfaces;
	vtable->vaDestroySurfaces = sunxi_cedrus_DestroySurfaces;
	vtable->vaCreateContext = sunxi_cedrus_CreateContext;
	vtable->vaDestroyContext = sunxi_cedrus_DestroyContext;
	vtable->vaCreateBuffer = sunxi_cedrus_CreateBuffer;
	vtable->vaBufferSetNumElements = sunxi_cedrus_BufferSetNumElements;
	vtable->vaMapBuffer = sunxi_cedrus_MapBuffer;
	vtable->vaUnmapBuffer = sunxi_cedrus_UnmapBuffer;
	vtable->vaDestroyBuffer = sunxi_cedrus_DestroyBuffer;
	vtable->vaBeginPicture = sunxi_cedrus_BeginPicture;
	vtable->vaRenderPicture = sunxi_cedrus_RenderPicture;
	vtable->vaEndPicture = sunxi_cedrus_EndPicture;
	vtable->vaSyncSurface = sunxi_cedrus_SyncSurface;
	vtable->vaQuerySurfaceStatus = sunxi_cedrus_QuerySurfaceStatus;
	vtable->vaPutSurface = sunxi_cedrus_PutSurface;
	vtable->vaQueryImageFormats = sunxi_cedrus_QueryImageFormats;
	vtable->vaCreateImage = sunxi_cedrus_CreateImage;
	vtable->vaDeriveImage = sunxi_cedrus_DeriveImage;
	vtable->vaDestroyImage = sunxi_cedrus_DestroyImage;
	vtable->vaSetImagePalette = sunxi_cedrus_SetImagePalette;
	vtable->vaGetImage = sunxi_cedrus_GetImage;
	vtable->vaPutImage = sunxi_cedrus_PutImage;
	vtable->vaQuerySubpictureFormats = sunxi_cedrus_QuerySubpictureFormats;
	vtable->vaCreateSubpicture = sunxi_cedrus_CreateSubpicture;
	vtable->vaDestroySubpicture = sunxi_cedrus_DestroySubpicture;
	vtable->vaSetSubpictureImage = sunxi_cedrus_SetSubpictureImage;
	vtable->vaSetSubpictureChromakey = sunxi_cedrus_SetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = sunxi_cedrus_SetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = sunxi_cedrus_AssociateSubpicture;
	vtable->vaDeassociateSubpicture = sunxi_cedrus_DeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = sunxi_cedrus_QueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = sunxi_cedrus_GetDisplayAttributes;
	vtable->vaSetDisplayAttributes = sunxi_cedrus_SetDisplayAttributes;
	vtable->vaLockSurface = sunxi_cedrus_LockSurface;
	vtable->vaUnlockSurface = sunxi_cedrus_UnlockSurface;
	vtable->vaBufferInfo = sunxi_cedrus_BufferInfo;

	driver_data = (struct sunxi_cedrus_driver_data *) malloc(sizeof(*driver_data));
	ctx->pDriverData = (void *) driver_data;

	assert(object_heap_init(&driver_data->config_heap, sizeof(struct object_config), CONFIG_ID_OFFSET)==0);
	assert(object_heap_init(&driver_data->context_heap, sizeof(struct object_context), CONTEXT_ID_OFFSET)==0);
	assert(object_heap_init(&driver_data->surface_heap, sizeof(struct object_surface), SURFACE_ID_OFFSET)==0);
	assert(object_heap_init(&driver_data->buffer_heap, sizeof(struct object_buffer), BUFFER_ID_OFFSET)==0);
	assert(object_heap_init(&driver_data->image_heap, sizeof(struct object_image), IMAGE_ID_OFFSET)==0);

	driver_data->mem2mem_fd = open("/dev/video0", O_RDWR | O_NONBLOCK, 0);
	assert(driver_data->mem2mem_fd >= 0);

	assert(ioctl(driver_data->mem2mem_fd, VIDIOC_QUERYCAP, &cap)==0);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE))
	{
		sunxi_cedrus_msg("/dev/video0 does not support m2m_mplane\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	return VA_STATUS_SUCCESS;
}

