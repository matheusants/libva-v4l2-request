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

#ifndef _CONTEXT_H_
#define _CONTEXT_H_

#include <va/va_backend.h>
#include <va/va_enc_h264.h>

#include "object_heap.h"
#include "h264.h"

#define CONTEXT(data, id)                                                      \
	((struct object_context *)object_heap_lookup(&(data)->context_heap, id))
#define CONTEXT_ID_OFFSET		0x02000000

struct request_data;
struct object_surface;

struct object_context {
	struct object_base base;

	VAConfigID config_id;
	VASurfaceID render_surface_id;
	VASurfaceID *surfaces_ids;
	int surfaces_count;

	int picture_width;
	int picture_height;
	int flags;

	/* H264 decode only */
	struct h264_dpb dpb;

	/*
	 * H264 encode (VAEntrypointEncSlice). The encoder is the standalone
	 * sunxi-venc stateful V4L2 M2M device (/dev/videoN), separate from the
	 * cedrus decoder — it has its own fd and needs no Request API.
	 */
	bool is_encoder;
	int encoder_fd;
	bool encoder_streaming;
	unsigned int enc_frame_num;

	/*
	 * One transient V4L2 buffer pair, reused every frame: the encode runs
	 * synchronously (blocking DQBUF) so a single in-flight frame suffices.
	 * Input surfaces are plain heap NV12 (filled by vaPutImage) and copied
	 * into enc_out_data per frame.
	 */
	unsigned int enc_out_index;
	void *enc_out_data;
	unsigned int enc_out_size;
	unsigned int enc_cap_index;
	void *enc_cap_data;
	unsigned int enc_cap_size;

	/* Encode parameters latched via RenderPicture for the current frame. */
	VAEncSequenceParameterBufferH264 enc_seq;
	bool enc_seq_valid;
	VAEncPictureParameterBufferH264 enc_pic;
	bool enc_pic_valid;
	/* Target bitrate from VAEncMiscParameterRateControl (bits/s). The
	 * sequence buffer's bits_per_second carries the VBR *peak*, not the
	 * target — using it overshoots, so the RC misc buffer wins. */
	unsigned int enc_rc_bitrate;
	bool enc_rc_valid;
};

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id);
VAStatus RequestDestroyContext(VADriverContextP context,
			       VAContextID context_id);

#endif
