/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
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

#include "context.h"
#include "config.h"
#include "request.h"
#include "surface.h"

#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "mpeg2-ctrls.h"
#include "h264-ctrls.h"
#include "hevc-ctrls.h"

#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_context *context_object = NULL;
	struct video_format *video_format;
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
        unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
        unsigned int destination_planes_count;
	unsigned int length;
	unsigned int offset;
	void *source_data = MAP_FAILED;
	VASurfaceID *ids = NULL;
	VAContextID id;
	VAStatus status;
	unsigned int output_type, capture_type;
	unsigned int pixelformat;
	unsigned int output_index_base;
        unsigned int capture_index_base;
	unsigned int index;
	unsigned int i, j;
	int rc;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);
	destination_planes_count = video_format->planes_count;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL) {
		status = VA_STATUS_ERROR_INVALID_CONFIG;
		goto error;
	}

	id = object_heap_allocate(&driver_data->context_heap);
	context_object = CONTEXT(driver_data, id);
	if (context_object == NULL) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}
	memset(&context_object->dpb, 0, sizeof(context_object->dpb));

	switch (config_object->profile) {

	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		pixelformat = V4L2_PIX_FMT_MPEG2_SLICE;
		break;

	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
		/* Altered from V4L2_PIX_FMT_H264_SLICE_RAW */
		pixelformat = V4L2_PIX_FMT_H264_SLICE;
		break;

	case VAProfileHEVCMain:
		pixelformat = V4L2_PIX_FMT_HEVC_SLICE;
		break;

	default:
		status = VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
		goto error;
	}

	/* To avoid S_FMT in CAPTURE (surface.c line 95) before OUTPUT
	BUFS are cleared
	DID'NT WORK - removing */
	//v4l2_request_buffers(driver_data->video_fd, capture_type, 0);

	rc = v4l2_set_format(driver_data->video_fd, output_type, pixelformat,
			     picture_width, picture_height);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*As BUFS were cleared, S_FMT for CAPTURE is set here after OUTPUT */
	rc = v4l2_set_format(driver_data->video_fd, capture_type,
                             video_format->v4l2_format,
                             picture_width, picture_height);
        if (rc < 0) {
                request_log("RequestCreateContext: S_FMT CAPTURE failed\n");
                status = VA_STATUS_ERROR_OPERATION_FAILED;
                goto error;
        }

	rc = v4l2_create_buffers(driver_data->video_fd, output_type,
				 surfaces_count, &output_index_base);
	if (rc < 0) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}

	/* CREATE_BUFS for CAPTURE */
        rc = v4l2_create_buffers(driver_data->video_fd, capture_type,
                                 surfaces_count, &capture_index_base);
        if (rc < 0) {
                request_log("RequestCreateContext: CREATE_BUFS OUTPUT failed\n");
                status = VA_STATUS_ERROR_ALLOCATION_FAILED;
                goto error;
        }

	request_log("RequestCreateContext: surfaces_count=%d "
	            "output_index_base=%u capture_index_base=%u\n",
        	    surfaces_count, output_index_base, capture_index_base);

	/* G_FMT CAPTURE to get bytesperline/sizes for mmap layout */
        rc = v4l2_get_format(driver_data->video_fd, capture_type, NULL, NULL,
                             destination_bytesperlines, destination_sizes, NULL);
        if (rc < 0) {
                request_log("RequestCreateContext: G_FMT CAPTURE failed\n");
                status = VA_STATUS_ERROR_OPERATION_FAILED;
                goto error;
        }

	/*
	 * The surface_ids array has been allocated by the caller and
	 * we don't have any indication wrt its life time. Let's make sure
	 * its life span is under our control.
	 */
	ids = malloc(surfaces_count * sizeof(VASurfaceID));
	if (ids == NULL) {
		status = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto error;
	}

	memcpy(ids, surfaces_ids, surfaces_count * sizeof(VASurfaceID));

	for (i = 0; i < surfaces_count; i++) {
		surface_object = SURFACE(driver_data, surfaces_ids[i]);
		if (surface_object == NULL) {
			status = VA_STATUS_ERROR_INVALID_SURFACE;
			goto error;
		}

		index = output_index_base + i;
		rc = v4l2_query_buffer(driver_data->video_fd, output_type,
				       index, &length, &offset, 1);
		if (rc < 0) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}

		source_data = mmap(NULL, length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, driver_data->video_fd, offset);
		if (source_data == MAP_FAILED) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}

		surface_object->source_index = index;
		surface_object->source_data = source_data;
		surface_object->source_size = length;

		index = capture_index_base + i;
                rc = v4l2_query_buffer(driver_data->video_fd, capture_type,
                                       index,
                                       surface_object->destination_map_lengths,
                                       surface_object->destination_map_offsets,
                                       video_format->v4l2_buffers_count);
                if (rc < 0) {
                        status = VA_STATUS_ERROR_ALLOCATION_FAILED;
                        goto error;
                }

		for (j = 0; j < video_format->v4l2_buffers_count; j++) {
                        surface_object->destination_map[j] =
                                mmap(NULL,
                                     surface_object->destination_map_lengths[j],
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     driver_data->video_fd,
                                     surface_object->destination_map_offsets[j]);
                        if (surface_object->destination_map[j] == MAP_FAILED) {
                                status = VA_STATUS_ERROR_ALLOCATION_FAILED;
                                goto error;
                        }
                }

                /* Compute logical plane layout inside the mmap'd buffer(s) */
		if (video_format->v4l2_buffers_count == 1) {
                	destination_sizes[0] = destination_bytesperlines[0] * picture_height;

                       	for (j = 1; j < destination_planes_count; j++)
                       		destination_sizes[j] = destination_sizes[0] / 2;

                        for (j = 0; j < destination_planes_count; j++) {
                                surface_object->destination_offsets[j] =
                                	j > 0 ? destination_sizes[j - 1] : 0;
                                surface_object->destination_data[j] =
                                        ((unsigned char *)surface_object->destination_map[0] +
                                         surface_object->destination_offsets[j]);
                                surface_object->destination_sizes[j] =
                                        destination_sizes[j];
                                surface_object->destination_bytesperlines[j] =
                                        destination_bytesperlines[0];
                        }
                 } else if (video_format->v4l2_buffers_count == destination_planes_count) {
                 	for (j = 0; j < destination_planes_count; j++) {
	                        surface_object->destination_offsets[j] = 0;
                                surface_object->destination_data[j] =
                                        surface_object->destination_map[j];
                                surface_object->destination_sizes[j] =
                                        destination_sizes[j];
                                surface_object->destination_bytesperlines[j] =
                                        destination_bytesperlines[j];
                        }
                } else {
                        return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }

		request_log("RequestCreateContext: surface_id=%d dst_idx=%u src_idx=%u\n",
		            surfaces_ids[i], index, output_index_base + i);

                surface_object->destination_index         = index;
                surface_object->destination_planes_count  = destination_planes_count;
                surface_object->destination_buffers_count = video_format->v4l2_buffers_count;
        }

	rc = v4l2_set_stream(driver_data->video_fd, output_type, true);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = v4l2_set_stream(driver_data->video_fd, capture_type, true);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->surfaces_ids = ids;
	context_object->surfaces_count = surfaces_count;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;

	*context_id = id;

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	if (source_data != MAP_FAILED)
		munmap(source_data, length);

	if (ids != NULL)
		free(ids);

	if (context_object != NULL)
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);

complete:
	return status;
}

VAStatus RequestDestroyContext(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	VAStatus status;
	int rc;

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	rc = v4l2_set_stream(driver_data->video_fd, output_type, false);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_set_stream(driver_data->video_fd, capture_type, false);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Buffers liberation */

	status = RequestDestroySurfaces(context, context_object->surfaces_ids,
					context_object->surfaces_count);
	if (status != VA_STATUS_SUCCESS)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	free(context_object->surfaces_ids);

	object_heap_free(&driver_data->context_heap,
			 (struct object_base *)context_object);

	rc = v4l2_request_buffers(driver_data->video_fd, output_type, 0);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_request_buffers(driver_data->video_fd, capture_type, 0);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return VA_STATUS_SUCCESS;
}
