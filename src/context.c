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
#include "picture.h"
#include "request.h"
#include "surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "mpeg2-ctrls.h"
#include "h264-ctrls.h"
#include "hevc-ctrls.h"

#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

/*
 * M4 — H264 encode (VAEntrypointEncSlice).
 *
 * The encoder is the standalone sunxi-venc stateful V4L2 M2M device, separate
 * from the cedrus decoder. Locate it by VIDIOC_QUERYCAP driver name (override
 * with LIBVA_V4L2_REQUEST_ENCODER_PATH).
 */
static int encoder_device_open(void)
{
	const char *env = getenv("LIBVA_V4L2_REQUEST_ENCODER_PATH");
	struct v4l2_capability cap;
	char path[32];
	int fd, i;

	if (env != NULL) {
		fd = open(env, O_RDWR);
		if (fd >= 0)
			return fd;
	}

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
		    strcmp((const char *)cap.driver, "sunxi-venc") == 0)
			return fd;

		close(fd);
	}

	return -1;
}

/*
 * Set up an encode context: open the sunxi-venc M2M device, configure
 * NV12 OUTPUT / H264 CAPTURE formats, allocate and mmap the per-surface
 * input and coded buffers. STREAMON is deferred to the first RequestEndPicture
 * so the V4L2 encoder controls can be programmed from the VAAPI parameters.
 */
static VAStatus request_create_encode_context(struct request_data *driver_data,
					      VAConfigID config_id,
					      int picture_width,
					      int picture_height, int flags,
					      VASurfaceID *surfaces_ids,
					      int surfaces_count,
					      VAContextID *context_id)
{
	struct object_context *context_object = NULL;
	unsigned int output_type = v4l2_type_video_output(false);
	unsigned int capture_type = v4l2_type_video_capture(false);
	unsigned int output_base, capture_base;
	unsigned int length, offset;
	VAContextID id;
	void *map;
	int fd, rc;

	(void)surfaces_ids;
	(void)surfaces_count;

	fd = encoder_device_open();
	if (fd < 0) {
		request_log("encode: sunxi-venc device not found\n");
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	rc = v4l2_set_format(fd, output_type, V4L2_PIX_FMT_NV12,
			     picture_width, picture_height);
	if (rc < 0) {
		request_log("encode: S_FMT OUTPUT NV12 failed\n");
		goto error_fd;
	}

	rc = v4l2_set_format(fd, capture_type, V4L2_PIX_FMT_H264,
			     picture_width, picture_height);
	if (rc < 0) {
		request_log("encode: S_FMT CAPTURE H264 failed\n");
		goto error_fd;
	}

	/*
	 * One transient buffer per queue — the encode is synchronous.
	 * (M2) OUTPUT is created in V4L2_MEMORY_DMABUF mode: the encoder
	 * receives the decoded NV12 frame as an imported dma-buf fd at QBUF
	 * time (sourced from cedrus CAPTURE) instead of an mmap'd backing
	 * buffer. This skips ~13 MB of memcpy per 4K frame in the encode
	 * hot path (see request_encode_picture). CAPTURE keeps MMAP — coded
	 * H264 is read back into a VACodedBuffer (~1 MB) on every frame.
	 */
	rc = v4l2_create_buffers_dmabuf(fd, output_type, 1, &output_base);
	if (rc < 0) {
		request_log("encode: CREATE_BUFS OUTPUT (dmabuf) failed\n");
		goto error_fd;
	}

	rc = v4l2_create_buffers(fd, capture_type, 1, &capture_base);
	if (rc < 0) {
		request_log("encode: CREATE_BUFS CAPTURE failed\n");
		goto error_fd;
	}

	id = object_heap_allocate(&driver_data->context_heap);
	context_object = CONTEXT(driver_data, id);
	if (context_object == NULL)
		goto error_fd;
	/* Zero everything after object_base — the base holds heap bookkeeping
	 * (id, next_free) that object_heap_allocate already set up. */
	memset(&context_object->base + 1, 0,
	       sizeof(*context_object) - sizeof(context_object->base));

	/* (M2) OUTPUT is dma-buf — no mmap. Compute the expected per-frame
	 * size from the negotiated S_FMT for sanity checks at QBUF time. */
	{
		unsigned int wq, hq, bpl, sz, pn;

		rc = v4l2_get_format(fd, output_type, &wq, &hq, &bpl, &sz, &pn);
		if (rc < 0)
			goto error_ctx;
		context_object->enc_out_index = output_base;
		context_object->enc_out_data = NULL;
		context_object->enc_out_size = sz;
	}

	rc = v4l2_query_buffer(fd, capture_type, capture_base, &length, &offset,
			       1);
	if (rc < 0)
		goto error_ctx;
	map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (map == MAP_FAILED)
		goto error_ctx;
	context_object->enc_cap_index = capture_base;
	context_object->enc_cap_data = map;
	context_object->enc_cap_size = length;

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->surfaces_ids = NULL;
	context_object->surfaces_count = 0;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;
	context_object->is_encoder = true;
	context_object->encoder_fd = fd;
	context_object->encoder_streaming = false;
	context_object->enc_pending = false;
	context_object->enc_pending_coded_buf = VA_INVALID_ID;
	context_object->enc_pending_surface_id = VA_INVALID_ID;

	request_log("encode: context %u ready, %dx%d, OUTPUT=%u CAPTURE=%u\n",
		    id, picture_width, picture_height, context_object->enc_out_size,
		    context_object->enc_cap_size);

	*context_id = id;
	return VA_STATUS_SUCCESS;

error_ctx:
	object_heap_free(&driver_data->context_heap,
			 (struct object_base *)context_object);
error_fd:
	close(fd);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

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

	/* H264 encode contexts use the separate sunxi-venc M2M encoder. */
	config_object = CONFIG(driver_data, config_id);
	if (config_object != NULL &&
	    config_object->entrypoint == VAEntrypointEncSlice)
		return request_create_encode_context(driver_data, config_id,
						     picture_width,
						     picture_height, flags,
						     surfaces_ids,
						     surfaces_count, context_id);

	/* Video post-processing — software NV12 scaler, no V4L2 device. */
	if (config_object != NULL &&
	    config_object->entrypoint == VAEntrypointVideoProc) {
		struct object_context *vpp_ctx;
		VAContextID vid;

		vid = object_heap_allocate(&driver_data->context_heap);
		vpp_ctx = CONTEXT(driver_data, vid);
		if (vpp_ctx == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		/* Zero everything past object_base — see encode context. */
		memset(&vpp_ctx->base + 1, 0,
		       sizeof(*vpp_ctx) - sizeof(vpp_ctx->base));
		vpp_ctx->config_id = config_id;
		vpp_ctx->render_surface_id = VA_INVALID_ID;
		vpp_ctx->vpp_input_surface_id = VA_INVALID_ID;
		vpp_ctx->picture_width = picture_width;
		vpp_ctx->picture_height = picture_height;
		vpp_ctx->flags = flags;
		vpp_ctx->is_vpp = true;
		*context_id = vid;
		return VA_STATUS_SUCCESS;
	}

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

	/*
	 * Set V4L2_CID_MPEG_VIDEO_H264_PROFILE for H264 contexts. The cedrus
	 * BSP T527 driver registers this control with default=MAIN(2). When
	 * decoding High Profile content (CABAC + 8x8 transform + ...) without
	 * setting this to HIGH(4), the VE applies Main-profile decoding logic
	 * to P/B slices and produces garbage. The I-frame decodes correctly
	 * because it does not depend on profile-specific inter-prediction
	 * pathways. Set ONCE at context creation (NOT per request).
	 */
	switch (config_object->profile) {
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh: {
		struct v4l2_control ctrl = { .id = V4L2_CID_MPEG_VIDEO_H264_PROFILE };
		switch (config_object->profile) {
		case VAProfileH264ConstrainedBaseline:
			ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
			break;
		case VAProfileH264Main:
			ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
		default:
			ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
			break;
		}
		if (ioctl(driver_data->video_fd, VIDIOC_S_CTRL, &ctrl) < 0)
			request_log("RequestCreateContext: S_CTRL H264_PROFILE=%d "
				    "failed: %s\n", ctrl.value, strerror(errno));
		break;
	}
	default:
		break;
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

	/* G_FMT CAPTURE to get bytesperline/sizes for mmap layout.
	 * fmt_height is the kernel-aligned height (e.g. ALIGN(1080,32)=1088
	 * for TILED), which determines where the chroma plane actually starts. */
	unsigned int fmt_height = 0;
        rc = v4l2_get_format(driver_data->video_fd, capture_type, NULL, &fmt_height,
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

                /* Export CAPTURE buffers as dma-buf and remap via dmabuf fd.
                 * dma-buf mmap may yield a cached/WC mapping (vs. Device/NC
                 * from video_fd mmap), enabling DMA_BUF_IOCTL_SYNC-coherent
                 * fast CPU reads after VE decode. Falls back to original map
                 * on EXPBUF or remap failure. */
		{
			int export_fds[VIDEO_MAX_PLANES];
			unsigned int k;
			for (k = 0; k < VIDEO_MAX_PLANES; k++)
				export_fds[k] = -1;
			if (v4l2_export_buffer(driver_data->video_fd, capture_type,
					       index, O_RDWR, export_fds,
					       video_format->v4l2_buffers_count) == 0) {
				for (k = 0; k < video_format->v4l2_buffers_count; k++) {
					surface_object->destination_dmabuf_fd[k] =
						export_fds[k];
					void *remap = mmap(NULL,
						surface_object->destination_map_lengths[k],
						PROT_READ | PROT_WRITE,
						MAP_SHARED, export_fds[k], 0);
					if (remap != MAP_FAILED) {
						munmap(surface_object->destination_map[k],
						       surface_object->destination_map_lengths[k]);
						surface_object->destination_map[k] = remap;
					}
				}
			}
		}

                /* Compute logical plane layout inside the mmap'd buffer(s) */
		if (video_format->v4l2_buffers_count == 1) {
			/* Use kernel-reported fmt_height (aligned) so chroma offset
			 * matches kernel's cedrus_buf_addr(plane=1) computation. */
                	destination_sizes[0] = destination_bytesperlines[0] * fmt_height;

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

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	/* VPP context: no V4L2 device, just free the heap object. */
	if (context_object->is_vpp) {
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);
		return VA_STATUS_SUCCESS;
	}

	/* Encode context: tear down the sunxi-venc encoder. */
	if (context_object->is_encoder) {
		/* (M4) Drain any deferred encode so the kernel-side OUTPUT
		 * dma-buf import is dropped before we close the encoder fd. */
		if (context_object->enc_pending)
			request_encode_drain_pending(driver_data, context_object);
		if (context_object->encoder_streaming) {
			v4l2_set_stream(context_object->encoder_fd,
					v4l2_type_video_output(false), false);
			v4l2_set_stream(context_object->encoder_fd,
					v4l2_type_video_capture(false), false);
		}
		if (context_object->encoder_fd >= 0)
			close(context_object->encoder_fd);
		free(context_object->surfaces_ids);
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);
		return VA_STATUS_SUCCESS;
	}

	video_format = driver_data->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

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
