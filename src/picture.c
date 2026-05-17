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

#include "picture.h"
#include "buffer.h"
#include "config.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include "h264.h"
#include "h264_enc.h"
#include "h265.h"
#include "mpeg2.h"

#include <assert.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <poll.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

static VAStatus codec_store_buffer(struct request_data *driver_data,
				   VAProfile profile,
				   struct object_surface *surface_object,
				   struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VASliceDataBufferType:
		/*
		 * Since there is no guarantee that the allocation
		 * order is the same as the submission order (via
		 * RenderPicture), we can't use a V4L2 buffer directly
		 * and have to copy from a regular buffer.
		 */
		memcpy(surface_object->source_data +
			       surface_object->slices_size,
		       buffer_object->data,
		       buffer_object->size * buffer_object->count);
		surface_object->slices_size +=
			buffer_object->size * buffer_object->count;
		surface_object->slices_count++;
		break;

	case VAPictureParameterBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.picture));
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surface_object->params.h264.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.picture));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.picture));
			break;

		default:
			break;
		}
		break;

	case VASliceParameterBufferType:
		switch (profile) {
		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			/* Multi-slice limitation: only the LAST slice's params
			 * are kept. Single-slice frames decode correctly. Multi-
			 * slice frames need per-slice device_run + ctrls cycles
			 * with a slice param array (not implemented). */
			memcpy(&surface_object->params.h264.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.slice));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.slice));
			break;

		default:
			break;
		}
		break;

	case VAIQMatrixBufferType:
		switch (profile) {
		case VAProfileMPEG2Simple:
		case VAProfileMPEG2Main:
			memcpy(&surface_object->params.mpeg2.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.mpeg2.iqmatrix));
			surface_object->params.mpeg2.iqmatrix_set = true;
			break;

		case VAProfileH264Main:
		case VAProfileH264High:
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264MultiviewHigh:
		case VAProfileH264StereoHigh:
			memcpy(&surface_object->params.h264.matrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.matrix));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.iqmatrix));
			surface_object->params.h265.iqmatrix_set = true;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus codec_set_controls(struct request_data *driver_data,
				   struct object_context *context,
				   VAProfile profile,
				   struct object_surface *surface_object)
{
	int rc;

	switch (profile) {
	case VAProfileMPEG2Simple:
	case VAProfileMPEG2Main:
		rc = mpeg2_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264MultiviewHigh:
	case VAProfileH264StereoHigh:
		rc = h264_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileHEVCMain:
		rc = h265_set_controls(driver_data, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	return VA_STATUS_SUCCESS;
}

/*
 * M4 — encode one frame on the sunxi-venc M2M encoder. Program the V4L2
 * controls from the latched VAAPI parameters, queue the raw NV12 input and a
 * coded-output buffer, wait for the encode, and copy the coded H264 into the
 * VA coded buffer. The encoder fd is blocking, so DQBUF waits for completion.
 */
static VAStatus request_encode_picture(struct request_data *driver_data,
				       struct object_context *context_object)
{
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_buffer *coded_object;
	unsigned int output_type = v4l2_type_video_output(false);
	unsigned int capture_type = v4l2_type_video_capture(false);
	unsigned int nv12_size, coded_bytes = 0;
	struct timeval timestamp;
	int fd = context_object->encoder_fd;
	int rc;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object = SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (!context_object->enc_pic_valid)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (surface_object->source_data == NULL) {
		request_log("encode: surface 0x%x was never uploaded\n",
			    context_object->render_surface_id);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	/* Program controls; on the first frame also STREAMON both queues. */
	h264_enc_set_controls(context_object, config_object->profile,
			      !context_object->encoder_streaming);

	if (!context_object->encoder_streaming) {
		rc = v4l2_set_stream(fd, output_type, true);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		rc = v4l2_set_stream(fd, capture_type, true);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		context_object->encoder_streaming = true;
	}

	nv12_size = context_object->picture_width *
		    context_object->picture_height * 3 / 2;
	if (nv12_size > context_object->enc_out_size)
		nv12_size = context_object->enc_out_size;
	if (nv12_size > surface_object->source_size)
		nv12_size = surface_object->source_size;

	/* Copy the uploaded NV12 frame into the encoder's OUTPUT buffer. */
	memcpy(context_object->enc_out_data, surface_object->source_data,
	       nv12_size);

	gettimeofday(&timestamp, NULL);

	/* Queue the coded-output buffer, then the raw NV12 input. */
	rc = v4l2_queue_buffer(fd, -1, capture_type, &timestamp,
			       context_object->enc_cap_index, 0, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_queue_buffer(fd, -1, output_type, &timestamp,
			       context_object->enc_out_index, nv12_size, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Blocking DQBUF — waits for the encode to finish. */
	if (v4l2_dequeue_buffer(fd, -1, output_type,
				context_object->enc_out_index, 1) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (v4l2_dequeue_buffer_size(fd, capture_type,
				     context_object->enc_cap_index, 1,
				     &coded_bytes) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Copy the coded H264 into the VA coded buffer. */
	coded_object = BUFFER(driver_data, context_object->enc_pic.coded_buf);
	if (coded_object == NULL ||
	    coded_object->type != VAEncCodedBufferType)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (coded_bytes > coded_object->size)
		coded_bytes = coded_object->size;
	memcpy(coded_object->data, context_object->enc_cap_data, coded_bytes);

	coded_object->coded_segment.buf = coded_object->data;
	coded_object->coded_segment.size = coded_bytes;
	coded_object->coded_segment.bit_offset = 0;
	coded_object->coded_segment.status = 0;
	coded_object->coded_segment.next = NULL;

	context_object->enc_frame_num++;
	surface_object->status = VASurfaceReady;
	context_object->render_surface_id = VA_INVALID_ID;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
                             VASurfaceID surface_id)
{
        struct request_data *driver_data = context->pDriverData;
        struct object_context *context_object;
        struct object_surface *surface_object;

        context_object = CONTEXT(driver_data, context_id);
        if (context_object == NULL)
                return VA_STATUS_ERROR_INVALID_CONTEXT;

        surface_object = SURFACE(driver_data, surface_id);
        if (surface_object == NULL)
                return VA_STATUS_ERROR_INVALID_SURFACE;

        /* Encode: no Request API, no DPB sync — just latch the surface. */
        if (context_object->is_encoder) {
                surface_object->status = VASurfaceRendering;
                context_object->render_surface_id = surface_id;
                return VA_STATUS_SUCCESS;
        }

        if (surface_object->status == VASurfaceRendering)
                RequestSyncSurface(context, surface_id);

        /* Se esta surface ainda tem buffer CAPTURE no estado DONE no kernel
         * (decodificação anterior concluída mas DQBUF propositalmente adiado
         * para manter a referência acessível ao driver BSP T527), precisamos
         * fazer o DQBUF agora, antes de re-enfileirar no próximo EndPicture.
         *
         * Isto é seguro porque RequestBeginPicture marca que esta surface
         * está prestes a ser sobrescrita — ela não será mais usada como
         * frame de referência pelo hardware para frames subsequentes.
         */
        if (surface_object->capture_queued) {
                unsigned int capture_type = v4l2_type_video_capture(
                        driver_data->video_format->v4l2_mplane);
                int dq_rc = v4l2_dequeue_buffer(driver_data->video_fd, -1,
                                                 capture_type,
                                                 surface_object->destination_index,
                                                 surface_object->destination_buffers_count);
                /* Only clear flag if DQBUF succeeded; on failure keep
                 * capture_queued=true to retry next time. */
                if (dq_rc >= 0)
                        surface_object->capture_queued = false;
        }

	if (surface_object->request_fd >= 0) {
		/* MEDIA_REQUEST_IOC_REINIT is a no-op on BSP T527.
		 * Close the old request_fd and allocate a new one in
		 * RequestEndPicture. */
		close(surface_object->request_fd);
		surface_object->request_fd = -1;
	}

        surface_object->status = VASurfaceRendering;
        context_object->render_surface_id = surface_id;
        return VA_STATUS_SUCCESS;
}

VAStatus RequestRenderPicture(VADriverContextP context, VAContextID context_id,
			      VABufferID *buffers_ids, int buffers_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	int rc;
	int i;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	/* Encode: latch the VAAPI encode parameter buffers for this frame. */
	if (context_object->is_encoder) {
		for (i = 0; i < buffers_count; i++) {
			buffer_object = BUFFER(driver_data, buffers_ids[i]);
			if (buffer_object == NULL)
				return VA_STATUS_ERROR_INVALID_BUFFER;

			switch (buffer_object->type) {
			case VAEncSequenceParameterBufferType:
				memcpy(&context_object->enc_seq,
				       buffer_object->data,
				       buffer_object->size <
					       sizeof(context_object->enc_seq) ?
				       buffer_object->size :
				       sizeof(context_object->enc_seq));
				context_object->enc_seq_valid = true;
				break;

			case VAEncPictureParameterBufferType:
				memcpy(&context_object->enc_pic,
				       buffer_object->data,
				       buffer_object->size <
					       sizeof(context_object->enc_pic) ?
				       buffer_object->size :
				       sizeof(context_object->enc_pic));
				context_object->enc_pic_valid = true;
				break;

			case VAEncMiscParameterBufferType: {
				/* Pull the target bitrate out of an RC misc
				 * buffer: target = peak * target_percentage. */
				VAEncMiscParameterBuffer *misc =
					buffer_object->data;

				if (misc != NULL && misc->type ==
				    VAEncMiscParameterTypeRateControl) {
					VAEncMiscParameterRateControl *rc =
						(VAEncMiscParameterRateControl *)
						misc->data;
					unsigned int pct = rc->target_percentage;

					if (pct == 0 || pct > 100)
						pct = 100;	/* CBR */
					context_object->enc_rc_bitrate =
						(unsigned int)
						((uint64_t)rc->bits_per_second *
						 pct / 100);
					context_object->enc_rc_valid = true;
				}
				break;
			}

			case VAEncSliceParameterBufferType:
				/* The VE emits slice data itself. */
				break;

			default:
				break;
			}
		}

		return VA_STATUS_SUCCESS;
	}

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	for (i = 0; i < buffers_count; i++) {
		buffer_object = BUFFER(driver_data, buffers_ids[i]);
		if (buffer_object == NULL)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		rc = codec_store_buffer(driver_data, config_object->profile,
					surface_object, buffer_object);
		if (rc != VA_STATUS_SUCCESS)
			return rc;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id)
{
        struct request_data *driver_data = context->pDriverData;
        struct object_context *context_object;
        struct object_config *config_object;
        struct object_surface *surface_object;
        struct video_format *video_format;
        unsigned int output_type, capture_type;
        int request_fd;
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

        /* Encode contexts run the sunxi-venc M2M path. */
        if (context_object->is_encoder)
                return request_encode_picture(driver_data, context_object);

        config_object = CONFIG(driver_data, context_object->config_id);
        if (config_object == NULL)
                return VA_STATUS_ERROR_INVALID_CONFIG;

        surface_object = SURFACE(driver_data, context_object->render_surface_id);
        if (surface_object == NULL)
                return VA_STATUS_ERROR_INVALID_SURFACE;

        request_fd = surface_object->request_fd;
        if (request_fd < 0) {
                request_fd = media_request_alloc(driver_data->media_fd);
                if (request_fd < 0)
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                surface_object->request_fd = request_fd;
        }

        /* Gerar o timestamp ANTES de tudo.
         * 1) dpb_insert (dentro de codec_set_controls) precisa do timestamp
         *    correto para gravar no dpb_entry — deve ser o mesmo que o kernel
         *    vai ver no vb2_buf.timestamp do buffer CAPTURE.
         * 2) O QBUF CAPTURE recebe este timestamp explicitamente para garantir
         *    que vb2_buf.timestamp seja definido corretamente — o BSP T527 pode
         *    não copiar automaticamente o timestamp do OUTPUT para o CAPTURE.
         * 3) O QBUF OUTPUT usa o mesmo timestamp para criar o par coerente. */
        gettimeofday(&surface_object->timestamp, NULL);

        rc = codec_set_controls(driver_data, context_object,
                                config_object->profile, surface_object);
        if (rc != VA_STATUS_SUCCESS)
                return rc;

        rc = v4l2_queue_buffer(driver_data->video_fd, -1, capture_type,
                                &surface_object->timestamp,
                                surface_object->destination_index, 0,
                                surface_object->destination_buffers_count);
        if (rc < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;
        surface_object->capture_queued = true;

        rc = v4l2_queue_buffer(driver_data->video_fd, request_fd, output_type,
                               &surface_object->timestamp,
                               surface_object->source_index,
                               surface_object->slices_size, 1);
        if (rc < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;

        surface_object->slices_size = 0;
        surface_object->slices_count = 0;

        status = RequestSyncSurface(context, context_object->render_surface_id);
        if (status != VA_STATUS_SUCCESS)
                return status;

        context_object->render_surface_id = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
}