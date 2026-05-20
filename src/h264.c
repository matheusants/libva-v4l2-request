/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
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

#include <assert.h>
#include <limits.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include "h264-ctrls.h"

#include "config.h"
#include "context.h"
#include "request.h"
#include "surface.h"
#include "v4l2.h"

#include "utils.h"
#include "h264.h"
#include "v4l2_translation.h"

enum h264_slice_type {
	H264_SLICE_P = 0,
	H264_SLICE_B = 1,
};

static bool is_picture_null(VAPictureH264 *pic)
{
	return pic->picture_id == VA_INVALID_SURFACE;
}

static struct h264_dpb_entry *dpb_find_invalid_entry(struct object_context *context)
{
	unsigned int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (!entry->valid && !entry->reserved)
			return entry;
	}

	return NULL;
}

static struct h264_dpb_entry *dpb_find_oldest_unused_entry(struct object_context *context)
{
	unsigned int min_age = UINT_MAX;
	unsigned int i;
	struct h264_dpb_entry *match = NULL;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (entry->reserved)
			continue;

		if (!entry->used && (entry->age < min_age)) {
			min_age = entry->age;
			match = entry;
		}
	}

	return match;
}

static struct h264_dpb_entry *dpb_find_entry(struct object_context *context)
{
	struct h264_dpb_entry *entry;

	entry = dpb_find_invalid_entry(context);
	if (!entry)
		entry = dpb_find_oldest_unused_entry(context);

	return entry;
}

static struct h264_dpb_entry *dpb_lookup(struct object_context *context,
										 VAPictureH264 *pic, unsigned int *idx)
{
	unsigned int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		if (!entry->valid)
			continue;

		if (entry->pic.picture_id == pic->picture_id) {
			if (idx)
				*idx = i;

			return entry;
		}
	}

	return NULL;
}

static void dpb_clear_entry(struct h264_dpb_entry *entry, bool reserved)
{
	VAPictureH264 saved_pic = entry->pic;

	memset(entry, 0, sizeof(*entry));

	if (reserved) {
		entry->reserved = true;
		entry->pic = saved_pic;
	}
}

static void dpb_insert(struct object_context *context, VAPictureH264 *pic,
		       struct h264_dpb_entry *entry, uint64_t timestamp)
{
	if (is_picture_null(pic))
		return;

	if (dpb_lookup(context, pic, NULL))
		return;

	if (!entry)
		entry = dpb_find_entry(context);

	memcpy(&entry->pic, pic, sizeof(entry->pic));
	entry->age = context->dpb.age;
	entry->valid = true;
	entry->reserved = false;
	/* Salva o timestamp do momento da inserção. Este é o valor que o
	 * kernel terá no vb2_buf.timestamp do buffer CAPTURE correspondente,
	 * e é o que o driver Cedrus usa em vb2_find_timestamp() para lookup
	 * de frames de referência. Não usar surface->timestamp mais tarde
	 * pois a surface pode ser reutilizada com um timestamp diferente. */
	entry->timestamp = timestamp;

	if (!(pic->flags & VA_PICTURE_H264_INVALID))
		entry->used = true;
}

static void dpb_update(struct request_data *driver_data,
					   struct object_context *context,
					   VAPictureParameterBufferH264 *parameters)
{
	unsigned int i;

	context->dpb.age++;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct h264_dpb_entry *entry = &context->dpb.entries[i];

		entry->used = false;
	}

	for (i = 0; i < parameters->num_ref_frames; i++) {
		VAPictureH264 *pic = &parameters->ReferenceFrames[i];
		struct h264_dpb_entry *entry;

		if (is_picture_null(pic))
			continue;

		/* Skip CurrPic if it appears in ReferenceFrames. Otherwise
		 * dpb_insert below would steal the slot reserved for the
		 * current frame's output, the kernel would see CurrPic in
		 * the DPB, vb2_find_timestamp would return the output buffer
		 * index, and motion comp would read from the buffer being
		 * written -> P/B frames decode as garbage. */
		if (pic->picture_id == parameters->CurrPic.picture_id)
			continue;

		entry = dpb_lookup(context, pic, NULL);
		if (entry) {
			entry->age = context->dpb.age;
			entry->used = true;
		} else {
			/* Frame de referência novo no DPB: capturar o timestamp
			 * atual da surface — neste momento ainda corresponde ao
			 * decode que gerou este frame de referência. */
			struct object_surface *ref_surface =
				SURFACE(driver_data, pic->picture_id);
			uint64_t ts = ref_surface ?
				v4l2_timeval_to_ns(&ref_surface->timestamp) : 0;
			dpb_insert(context, pic, NULL, ts);
		}
	}
}

static void h264_fill_dpb(struct object_context *context,
			  			  struct v4l2_ctrl_h264_decode_params_internal *decode)
{
	int i;

	for (i = 0; i < H264_DPB_SIZE; i++) {
		struct v4l2_h264_dpb_entry_internal *dpb = &decode->dpb[i];
		struct h264_dpb_entry *entry = &context->dpb.entries[i];
		uint64_t timestamp;

		if (!entry->valid)
			continue;

		/* Usar o timestamp salvo no dpb_entry no momento da inserção.
		 * NÃO usar surface->timestamp pois a surface pode ter sido
		 * reutilizada como destino de um frame mais recente, o que
		 * atualizaria surface->timestamp e quebraria o lookup no driver
		 * Cedrus (vb2_find_timestamp busca pelo timestamp do decode
		 * que gerou o frame de referência, não o mais recente). */
		timestamp = entry->timestamp;
		dpb->reference_ts = timestamp;

		dpb->frame_num = entry->pic.frame_idx;
		dpb->top_field_order_cnt = entry->pic.TopFieldOrderCnt;
		dpb->bottom_field_order_cnt = entry->pic.BottomFieldOrderCnt;

		dpb->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;

		if (entry->used)
			dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;

		if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
			dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;

	}
}

static void h264_va_picture_to_v4l2(struct request_data *driver_data,
									struct object_context *context,
									struct object_surface *surface,
									VAPictureParameterBufferH264 *VAPicture,
									struct v4l2_ctrl_h264_decode_params_internal *decode,
									struct v4l2_ctrl_h264_pps_internal *pps,
									struct v4l2_ctrl_h264_sps_internal *sps)
{
	h264_fill_dpb(context, decode);

	decode->num_slices = surface->slices_count;
	decode->top_field_order_cnt = VAPicture->CurrPic.TopFieldOrderCnt;
	decode->bottom_field_order_cnt = VAPicture->CurrPic.BottomFieldOrderCnt;

	/*
	 * Derivar nal_ref_idc do CurrPic.flags. cedrus_set_params seta
	 * BIT(12) do VE_H264_SHS quando nal_ref_idc != 0 (cedrus_h264.c:423).
	 * Sem isso, o VE trata o frame como non-reference e os dados de
	 * referência (mv_col_buf, etc.) ficam corrompidos para frames P/B
	 * subsequentes — apenas o I-frame decodifica corretamente.
	 */
	if (VAPicture->CurrPic.flags & (VA_PICTURE_H264_LONG_TERM_REFERENCE |
					VA_PICTURE_H264_SHORT_TERM_REFERENCE))
		decode->nal_ref_idc = 1;
	else
		decode->nal_ref_idc = 0;

	pps->weighted_bipred_idc =
		VAPicture->pic_fields.bits.weighted_bipred_idc;
	pps->pic_init_qs_minus26 = VAPicture->pic_init_qs_minus26;
	pps->pic_init_qp_minus26 = VAPicture->pic_init_qp_minus26;
	pps->chroma_qp_index_offset = VAPicture->chroma_qp_index_offset;
	pps->second_chroma_qp_index_offset =
		VAPicture->second_chroma_qp_index_offset;

	if (VAPicture->pic_fields.bits.entropy_coding_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	if (VAPicture->pic_fields.bits.weighted_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;

	if (VAPicture->pic_fields.bits.transform_8x8_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	if (VAPicture->pic_fields.bits.constrained_intra_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;

	if (VAPicture->pic_fields.bits.pic_order_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;

	if (VAPicture->pic_fields.bits.deblocking_filter_control_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	if (VAPicture->pic_fields.bits.redundant_pic_cnt_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

	sps->chroma_format_idc = VAPicture->seq_fields.bits.chroma_format_idc;
	sps->bit_depth_luma_minus8 = VAPicture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 = VAPicture->bit_depth_chroma_minus8;
	sps->max_num_ref_frames = VAPicture->num_ref_frames;
	{
		struct object_config *config = CONFIG(driver_data, context->config_id);
		if (config) {
			switch (config->profile) {
			case VAProfileH264ConstrainedBaseline:
				sps->profile_idc = 66;
				sps->constraint_set_flags = 0x40;
				break;
			case VAProfileH264Main:
				sps->profile_idc = 77;
				break;
			case VAProfileH264High:
				sps->profile_idc = 100;
				break;
			case VAProfileH264MultiviewHigh:
				sps->profile_idc = 118;
				break;
			case VAProfileH264StereoHigh:
				sps->profile_idc = 128;
				break;
			default:
				sps->profile_idc = 0;
				break;
			}
		}
	}
	sps->log2_max_frame_num_minus4 =
		VAPicture->seq_fields.bits.log2_max_frame_num_minus4;
	sps->log2_max_pic_order_cnt_lsb_minus4 =
		VAPicture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
	sps->pic_order_cnt_type = VAPicture->seq_fields.bits.pic_order_cnt_type;
	sps->pic_width_in_mbs_minus1 = VAPicture->picture_width_in_mbs_minus1;
	sps->pic_height_in_map_units_minus1 =
		VAPicture->picture_height_in_mbs_minus1;

	if (VAPicture->seq_fields.bits.residual_colour_transform_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	if (VAPicture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
		sps->flags |=
			V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	if (VAPicture->seq_fields.bits.frame_mbs_only_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (VAPicture->seq_fields.bits.mb_adaptive_frame_field_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (VAPicture->seq_fields.bits.direct_8x8_inference_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	if (VAPicture->seq_fields.bits.delta_pic_order_always_zero_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
}

static void h264_va_matrix_to_v4l2(struct request_data *driver_data,
								   struct object_context *context,
								   VAIQMatrixBufferH264 *VAMatrix,
								   struct v4l2_ctrl_h264_scaling_matrix_internal *v4l2_matrix)
{
	memcpy(v4l2_matrix->scaling_list_4x4, &VAMatrix->ScalingList4x4,
	       sizeof(VAMatrix->ScalingList4x4));

	/*
	 * In YUV422, there's only two matrices involved, while YUV444
	 * needs 6. However, in the former case, the two matrices
	 * should be placed at the 0 and 3 offsets.
	 */
	memcpy(v4l2_matrix->scaling_list_8x8[0], &VAMatrix->ScalingList8x8[0],
	       sizeof(v4l2_matrix->scaling_list_8x8[0]));
	memcpy(v4l2_matrix->scaling_list_8x8[3], &VAMatrix->ScalingList8x8[1],
	       sizeof(v4l2_matrix->scaling_list_8x8[3]));
}

static void h264_copy_pred_table(struct v4l2_h264_weight_factors_internal *factors,
								 unsigned int num_refs,
								 int16_t luma_weight[32],
								 int16_t luma_offset[32],
								 int16_t chroma_weight[32][2],
								 int16_t chroma_offset[32][2])
{
	unsigned int i;

	for (i = 0; i < num_refs; i++) {
		unsigned int j;

		factors->luma_weight[i] = luma_weight[i];
		factors->luma_offset[i] = luma_offset[i];

		for (j = 0; j < 2; j++) {
			factors->chroma_weight[i][j] = chroma_weight[i][j];
			factors->chroma_offset[i][j] = chroma_offset[i][j];
		}
	}
}

static void h264_va_slice_to_v4l2(struct request_data *driver_data,
								  struct object_context *context,
								  VASliceParameterBufferH264 *VASlice,
								  VAPictureParameterBufferH264 *VAPicture,
								  struct v4l2_ctrl_h264_slice_params_internal *slice)
{
	/* Garantir estrutura limpa */
	memset(slice, 0, sizeof(*slice));

	slice->size = VASlice->slice_data_size;
	slice->header_bit_size = VASlice->slice_data_bit_offset;
	slice->first_mb_in_slice = VASlice->first_mb_in_slice;

	/* frame_num from picture params — only field translatable from
	 * VA-API. idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt_*,
	 * dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
	 * slice_group_change_cycle parsed from slice header bitstream by
	 * ffmpeg but not exposed via VA-API → stay 0. cedrus_h264.c does
	 * not read these fields, so 0 is safe. */
	slice->frame_num = VAPicture->frame_num;
	//slice->slice_type = VASlice->slice_type;

	/* Os valores de slice_type do VAAPI coincidem com os do V4L2 BSP:
	 * 0=P, 1=B, 2=I, 3=SP, 4=SI. Passar diretamente (% 5 para
	 * normalizar os tipos "todas as slices são do mesmo tipo" 5-9). */
	slice->slice_type = VASlice->slice_type % 5;

	slice->cabac_init_idc = VASlice->cabac_init_idc;
	slice->slice_qp_delta = VASlice->slice_qp_delta;
	slice->disable_deblocking_filter_idc =
		VASlice->disable_deblocking_filter_idc;
	slice->slice_alpha_c0_offset_div2 = VASlice->slice_alpha_c0_offset_div2;
	slice->slice_beta_offset_div2 = VASlice->slice_beta_offset_div2;

	/* Sempre inicializar */
	slice->num_ref_idx_l0_active_minus1 =
		VASlice->num_ref_idx_l0_active_minus1;
	slice->num_ref_idx_l1_active_minus1 =
		VASlice->num_ref_idx_l1_active_minus1;

	if (((VASlice->slice_type % 5) == H264_SLICE_P) ||
	    ((VASlice->slice_type % 5) == H264_SLICE_B)) {
		unsigned int i;
		
		unsigned int max = VASlice->num_ref_idx_l0_active_minus1 + 1;

		if (max > 32)
			max = 32;

		for (i = 0; i < max; i++) {
			VAPictureH264 *pic = &VASlice->RefPicList0[i];
			struct h264_dpb_entry *entry;
			unsigned int idx;

			entry = dpb_lookup(context, pic, &idx);
			if (!entry)
				continue;

			slice->ref_pic_list0[i] = idx;
		}
	}

	if ((VASlice->slice_type % 5) == H264_SLICE_B) {
		unsigned int i;

		unsigned int max = VASlice->num_ref_idx_l1_active_minus1 + 1;

		if (max > 32)
			max = 32;

		for (i = 0; i < max; i++) {
			VAPictureH264 *pic = &VASlice->RefPicList1[i];
			struct h264_dpb_entry *entry;
			unsigned int idx;

			entry = dpb_lookup(context, pic, &idx);
			if (!entry)
				continue;

			slice->ref_pic_list1[i] = idx;
		}
	}

	if (VASlice->direct_spatial_mv_pred_flag)
		slice->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;

	slice->pred_weight_table.chroma_log2_weight_denom =
		VASlice->chroma_log2_weight_denom;
	slice->pred_weight_table.luma_log2_weight_denom =
		VASlice->luma_log2_weight_denom;

	if (((VASlice->slice_type % 5) == H264_SLICE_P) ||
	    ((VASlice->slice_type % 5) == H264_SLICE_B))
		h264_copy_pred_table(&slice->pred_weight_table.weight_factors[0],
				     slice->num_ref_idx_l0_active_minus1 + 1,
				     VASlice->luma_weight_l0,
				     VASlice->luma_offset_l0,
				     VASlice->chroma_weight_l0,
				     VASlice->chroma_offset_l0);

	if ((VASlice->slice_type % 5) == H264_SLICE_B)
		h264_copy_pred_table(&slice->pred_weight_table.weight_factors[1],
				     slice->num_ref_idx_l1_active_minus1 + 1,
				     VASlice->luma_weight_l1,
				     VASlice->luma_offset_l1,
				     VASlice->chroma_weight_l1,
				     VASlice->chroma_offset_l1);

}

int h264_set_controls(struct request_data *driver_data,
		      struct object_context *context,
		      struct object_surface *surface)
{
	struct v4l2_ctrl_h264_scaling_matrix_internal matrix = { 0 };
	struct v4l2_ctrl_h264_decode_params_internal decode = { 0 };
	struct v4l2_ctrl_h264_slice_params_internal slice = { 0 };
	struct v4l2_ctrl_h264_pps_internal pps = { 0 };
	struct v4l2_ctrl_h264_sps_internal sps = { 0 };
	struct h264_dpb_entry *output;
	int rc;

	output = dpb_lookup(context, &surface->params.h264.picture.CurrPic,
			    NULL);
	if (!output)
		output = dpb_find_entry(context);

	dpb_clear_entry(output, true);

	dpb_update(driver_data, context, &surface->params.h264.picture);

	h264_va_picture_to_v4l2(driver_data, context, surface,
							&surface->params.h264.picture, &decode, &pps,
							&sps);
	h264_va_matrix_to_v4l2(driver_data, context,
			      		   &surface->params.h264.matrix, &matrix);
	h264_va_slice_to_v4l2(driver_data, context, &surface->params.h264.slice,
			      		  &surface->params.h264.picture, &slice);

	/*
	 * Traduzir structs internas (_internal, formato antigo Bootlin/staging)
	 * para as structs modernas que o kernel T527/Cedrus BSP 5.15 espera,
	 * e enviar via VIDIOC_S_EXT_CTRLS com os CIDs STATELESS corretos.
	 *
	 * Tamanhos antigos → novos:
	 *   SPS:    1048 → 1048 (idêntico, apenas renomeado)
	 *   PPS:      12 →   12 (idêntico)
	 *   MATRIX:  480 →  480 (idêntico)
	 *   SLICE:   892 →  152 (tradução campo a campo necessária)
	 *   DECODE:  496 →  624 (tradução campo a campo necessária)
	 */

	rc = h264_translate_and_set_controls(driver_data,
										 driver_data->video_fd,
										 surface->request_fd, &sps,
										 &pps, &matrix, &slice, &decode);

	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* Inserir o frame atual no DPB com o timestamp que acabou de ser
	 * setado pelo gettimeofday em EndPicture (antes do QBUF OUTPUT).
	 * Este é o mesmo timestamp que o kernel usará no vb2_buf.timestamp
	 * do buffer CAPTURE, e é o que vb2_find_timestamp() vai buscar. */
	
	dpb_insert(context, &surface->params.h264.picture.CurrPic, output,
		   v4l2_timeval_to_ns(&surface->timestamp));

	return VA_STATUS_SUCCESS;
}