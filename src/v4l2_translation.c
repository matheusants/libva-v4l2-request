/*
 * v4l2_translation.c — Tradução structs H.264 internas → structs modernas
 *
 * Converte as structs "antigas" (Bootlin/staging, sufixo _internal) para as
 * structs modernas de <linux/v4l2-controls.h>, que o kernel T527/Cedrus
 * BSP 5.15 espera via VIDIOC_S_EXT_CTRLS.
 *
 * Tamanhos confirmados com os headers do sistema (aarch64):
 *   SPS:          sizeof(v4l2_ctrl_h264_sps)             = 1048
 *   PPS:          sizeof(v4l2_ctrl_h264_pps)             =   12
 *   MATRIX:       sizeof(v4l2_ctrl_h264_scaling_matrix)  =  480
 *   PRED_WEIGHTS: sizeof(v4l2_ctrl_h264_pred_weights)    =  772  ← NOVO CID
 *   SLICE:        sizeof(v4l2_ctrl_h264_slice_params)    =  152
 *   DECODE:       sizeof(v4l2_ctrl_h264_decode_params)   =  560
 *   dpb_entry:    sizeof(v4l2_h264_dpb_entry)            =   32
 *   reference:    sizeof(v4l2_h264_reference)            =    2
 *   weight_factors: sizeof(v4l2_h264_weight_factors)     =  384
 *
 * MUDANÇAS CRÍTICAS vs formato antigo:
 *
 * 1) pred_weight_table: no formato antigo estava EMBUTIDA dentro de
 *    v4l2_ctrl_h264_slice_params_internal (892 bytes). Na API moderna
 *    ela é um controle SEPARADO: V4L2_CID_STATELESS_H264_PRED_WEIGHTS
 *    com struct v4l2_ctrl_h264_pred_weights (772 bytes). Deve ser enviada
 *    APENAS quando V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, slice) for
 *    verdadeiro (slices P com weighted_pred ou slices B com
 *    weighted_bipred_idc==1).
 *
 * 2) v4l2_h264_dpb_entry: layout reordenado (24→32 bytes):
 *    Antigo: {u64 ref_ts, u16 frame_num, u16 pic_num, s32 top, s32 bot, u32 flags}
 *    Moderno:{u64 ref_ts, u32 pic_num,   u16 frame_num, u8 fields, u8 reserved[5],
 *             s32 top, s32 bot, u32 flags}
 *
 * 3) v4l2_ctrl_h264_slice_params moderna NÃO tem pred_weight_table.
 *    Campos que "migraram" do slice antigo para decode_params moderno:
 *    frame_num, idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt*,
 *    dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *    slice_group_change_cycle.
 */

#include <string.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "h264-ctrls.h"
#include "v4l2_translation.h"
#include "v4l2.h"
#include "utils.h"

/* -----------------------------------------------------------------------
 * translate_sps() — 1048→1048, layout idêntico
 * ----------------------------------------------------------------------- */
static void translate_sps(const struct v4l2_ctrl_h264_sps_internal *src,
			   struct v4l2_ctrl_h264_sps *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->profile_idc                        = src->profile_idc;
	dst->constraint_set_flags               = src->constraint_set_flags;
	dst->level_idc                          = src->level_idc;
	dst->seq_parameter_set_id               = src->seq_parameter_set_id;
	dst->chroma_format_idc                  = src->chroma_format_idc;
	dst->bit_depth_luma_minus8              = src->bit_depth_luma_minus8;
	dst->bit_depth_chroma_minus8            = src->bit_depth_chroma_minus8;
	dst->log2_max_frame_num_minus4          = src->log2_max_frame_num_minus4;
	dst->pic_order_cnt_type                 = src->pic_order_cnt_type;
	dst->log2_max_pic_order_cnt_lsb_minus4  = src->log2_max_pic_order_cnt_lsb_minus4;
	dst->max_num_ref_frames                 = src->max_num_ref_frames;
	dst->num_ref_frames_in_pic_order_cnt_cycle =
		src->num_ref_frames_in_pic_order_cnt_cycle;
	memcpy(dst->offset_for_ref_frame, src->offset_for_ref_frame,
	       sizeof(dst->offset_for_ref_frame));
	dst->offset_for_non_ref_pic             = src->offset_for_non_ref_pic;
	dst->offset_for_top_to_bottom_field     = src->offset_for_top_to_bottom_field;
	dst->pic_width_in_mbs_minus1            = src->pic_width_in_mbs_minus1;
	dst->pic_height_in_map_units_minus1     = src->pic_height_in_map_units_minus1;
	dst->flags                              = src->flags;
}

/* -----------------------------------------------------------------------
 * translate_pps() — 12→12, layout idêntico
 * ----------------------------------------------------------------------- */
static void translate_pps(const struct v4l2_ctrl_h264_pps_internal *src,
			   struct v4l2_ctrl_h264_pps *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->pic_parameter_set_id                 = src->pic_parameter_set_id;
	dst->seq_parameter_set_id                 = src->seq_parameter_set_id;
	dst->num_slice_groups_minus1              = src->num_slice_groups_minus1;
	dst->num_ref_idx_l0_default_active_minus1 = src->num_ref_idx_l0_default_active_minus1;
	dst->num_ref_idx_l1_default_active_minus1 = src->num_ref_idx_l1_default_active_minus1;
	dst->weighted_bipred_idc                  = src->weighted_bipred_idc;
	dst->pic_init_qp_minus26                  = src->pic_init_qp_minus26;
	dst->pic_init_qs_minus26                  = src->pic_init_qs_minus26;
	dst->chroma_qp_index_offset               = src->chroma_qp_index_offset;
	dst->second_chroma_qp_index_offset        = src->second_chroma_qp_index_offset;
	/* Valores numéricos dos flags idênticos entre versões */
	dst->flags                                = src->flags;
}

/* -----------------------------------------------------------------------
 * translate_scaling_matrix() — 480→480, layout idêntico
 * ----------------------------------------------------------------------- */
static void translate_scaling_matrix(
	const struct v4l2_ctrl_h264_scaling_matrix_internal *src,
	struct v4l2_ctrl_h264_scaling_matrix *dst)
{
	memset(dst, 0, sizeof(*dst));
	memcpy(dst->scaling_list_4x4, src->scaling_list_4x4,
	       sizeof(dst->scaling_list_4x4));
	memcpy(dst->scaling_list_8x8, src->scaling_list_8x8,
	       sizeof(dst->scaling_list_8x8));
}

/* -----------------------------------------------------------------------
 * translate_pred_weights()
 *
 * Extrai a pred_weight_table do slice_params antigo (onde estava embutida)
 * e preenche a struct moderna independente v4l2_ctrl_h264_pred_weights
 * (772 bytes = 2×u16 + 2×v4l2_h264_weight_factors de 384 bytes cada).
 *
 * v4l2_h264_weight_factors moderno é idêntico ao v4l2_h264_weight_factors
 * do antigo (mesmo layout: luma_weight[32], luma_offset[32],
 * chroma_weight[32][2], chroma_offset[32][2]).
 *
 * Esta struct é enviada como controle SEPARADO apenas quando a macro
 * V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED indica que é necessário.
 * ----------------------------------------------------------------------- */
static void translate_pred_weights(
	const struct v4l2_ctrl_h264_slice_params_internal *slice_src,
	struct v4l2_ctrl_h264_pred_weights *dst)
{
	const struct v4l2_h264_pred_weight_table_internal *src =
		&slice_src->pred_weight_table;

	memset(dst, 0, sizeof(*dst));

	dst->luma_log2_weight_denom   = src->luma_log2_weight_denom;
	dst->chroma_log2_weight_denom = src->chroma_log2_weight_denom;

	/*
	 * v4l2_h264_weight_factors antigo (v4l2_h264_weight_factors_internal)
	 * e moderno (v4l2_h264_weight_factors) têm layout idêntico:
	 *   s16 luma_weight[32], luma_offset[32]
	 *   s16 chroma_weight[32][2], chroma_offset[32][2]
	 * Cópia direta via memcpy é segura.
	 */
	memcpy(&dst->weight_factors[0], &src->weight_factors[0],
	       sizeof(dst->weight_factors[0]));
	memcpy(&dst->weight_factors[1], &src->weight_factors[1],
	       sizeof(dst->weight_factors[1]));
}

/* -----------------------------------------------------------------------
 * translate_slice_params()
 *
 * Slice params interno (892 bytes) → moderno (152 bytes).
 *
 * A struct moderna NÃO contém pred_weight_table — ela se tornou controle
 * independente V4L2_CID_STATELESS_H264_PRED_WEIGHTS (tratado separado).
 *
 * Campos descartados do antigo (ausentes no moderno de slice):
 *   size, pred_weight_table, pic_parameter_set_id, frame_num,
 *   idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt_bottom/0/1,
 *   dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *   slice_group_change_cycle.
 *   (Os campos de POC/marking migraram para decode_params moderno.)
 *
 * slice_qs_delta: PRESENTE no moderno (era descartado na versão anterior
 * deste arquivo por engano) — restaurado.
 *
 * ref_pic_list0/1: __u8[32] → struct v4l2_h264_reference[32]
 *   .index = old[i], .fields = V4L2_H264_FRAME_REF (0x3)
 *
 * Flags: valores numéricos diferem entre versões:
 *   Antigo: FIELD_PIC=0x01, BOTTOM_FIELD=0x02,
 *           DIRECT_SPATIAL_MV_PRED=0x04, SP_FOR_SWITCH=0x08
 *   Moderno: DIRECT_SPATIAL_MV_PRED=0x01, SP_FOR_SWITCH=0x02
 * ----------------------------------------------------------------------- */
static void translate_slice_params(
	const struct v4l2_ctrl_h264_slice_params_internal *src,
	struct v4l2_ctrl_h264_slice_params *dst)
{
	unsigned int i, max_l0, max_l1;

	memset(dst, 0, sizeof(*dst));

	dst->header_bit_size               = src->header_bit_size;
	dst->first_mb_in_slice             = (__u32)src->first_mb_in_slice;
	dst->slice_type                    = src->slice_type;
	dst->colour_plane_id               = src->colour_plane_id;
	dst->redundant_pic_cnt             = src->redundant_pic_cnt;
	dst->cabac_init_idc                = src->cabac_init_idc;
	dst->slice_qp_delta                = src->slice_qp_delta;
	dst->slice_qs_delta                = src->slice_qs_delta;  /* presente no moderno */
	dst->disable_deblocking_filter_idc = src->disable_deblocking_filter_idc;
	dst->slice_alpha_c0_offset_div2    = src->slice_alpha_c0_offset_div2;
	dst->slice_beta_offset_div2        = src->slice_beta_offset_div2;
	dst->num_ref_idx_l0_active_minus1  = src->num_ref_idx_l0_active_minus1;
	dst->num_ref_idx_l1_active_minus1  = src->num_ref_idx_l1_active_minus1;
	/* dst->reserved = 0  (garantido pelo memset) */

	/*
	 * ref_pic_list0: __u8[32] → struct v4l2_h264_reference[32]
	 * V4L2_H264_FRAME_REF = 0x3 (top + bottom field) para frames
	 * progressivos. Traduzir apenas as entradas ativas.
	 */
	max_l0 = (unsigned int)src->num_ref_idx_l0_active_minus1 + 1;
	if (max_l0 > 32)
		max_l0 = 32;
	for (i = 0; i < max_l0; i++) {
		dst->ref_pic_list0[i].index  = src->ref_pic_list0[i];
		dst->ref_pic_list0[i].fields = V4L2_H264_FRAME_REF;
	}

	/* ref_pic_list1 apenas para slices B */
	if ((src->slice_type % 5) == V4L2_H264_SLICE_TYPE_B) {
		max_l1 = (unsigned int)src->num_ref_idx_l1_active_minus1 + 1;
		if (max_l1 > 32)
			max_l1 = 32;
		for (i = 0; i < max_l1; i++) {
			dst->ref_pic_list1[i].index  = src->ref_pic_list1[i];
			dst->ref_pic_list1[i].fields = V4L2_H264_FRAME_REF;
		}
	}

	/*
	 * Mapear flags com conversão explícita de valor numérico.
	 * DIRECT_SPATIAL_MV_PRED: antigo=0x04 → moderno=0x01
	 * SP_FOR_SWITCH:           antigo=0x08 → moderno=0x02
	 * FIELD_PIC (0x01) e BOTTOM_FIELD (0x02) do antigo não existem
	 * na struct moderna de slice.
	 */
	dst->flags = 0;
	if (src->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)
		dst->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
	if (src->flags & V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH)
		dst->flags |= V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH;
}

/* -----------------------------------------------------------------------
 * translate_decode_params()
 *
 * Decode params interno (496 bytes) → moderno (560 bytes).
 *
 * dpb_entry: 24→32 bytes (reordenação pic_num/frame_num + fields + reserved).
 *
 * @slice_src: campos que "migraram" do slice_params antigo para
 *             decode_params moderno são extraídos daqui:
 *             frame_num, idr_pic_id, pic_order_cnt_lsb,
 *             delta_pic_order_cnt_bottom/0/1,
 *             dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *             slice_group_change_cycle.
 * ----------------------------------------------------------------------- */
static void translate_decode_params(
	const struct v4l2_ctrl_h264_decode_params_internal *src,
	const struct v4l2_ctrl_h264_slice_params_internal  *slice_src,
	struct v4l2_ctrl_h264_decode_params *dst)
{
	unsigned int i;

	memset(dst, 0, sizeof(*dst));

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry_internal *esrc = &src->dpb[i];
		struct v4l2_h264_dpb_entry              *edst = &dst->dpb[i];

		edst->reference_ts           = esrc->reference_ts;
		edst->pic_num                = (__u32)esrc->pic_num;   /* u16→u32 */
		edst->frame_num              = esrc->frame_num;
		/*
		 * fields: V4L2_H264_FRAME_REF=0x3 para entradas válidas
		 * de streams progressivos. Entradas inválidas ficam 0.
		 */
		edst->fields = (esrc->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)
			? V4L2_H264_FRAME_REF : 0;
		/* reserved[5] = 0 garantido pelo memset */
		edst->top_field_order_cnt    = esrc->top_field_order_cnt;
		edst->bottom_field_order_cnt = esrc->bottom_field_order_cnt;
		/* VALID=0x01, ACTIVE=0x02, LONG_TERM=0x04 — mesmos valores */
		edst->flags = esrc->flags &
			(V4L2_H264_DPB_ENTRY_FLAG_VALID  |
			 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
			 V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM);
		/* FIELD=0x08 não existe no formato antigo → fica 0 */
	}

	/* Campos presentes no decode_params antigo */
	dst->nal_ref_idc            = src->nal_ref_idc;
	dst->top_field_order_cnt    = src->top_field_order_cnt;
	dst->bottom_field_order_cnt = src->bottom_field_order_cnt;

	/* Campos que migraram do slice_params antigo para decode_params moderno */
	dst->frame_num                    = slice_src->frame_num;
	dst->idr_pic_id                   = slice_src->idr_pic_id;
	dst->pic_order_cnt_lsb            = slice_src->pic_order_cnt_lsb;
	dst->delta_pic_order_cnt_bottom   = slice_src->delta_pic_order_cnt_bottom;
	dst->delta_pic_order_cnt0         = slice_src->delta_pic_order_cnt0;
	dst->delta_pic_order_cnt1         = slice_src->delta_pic_order_cnt1;
	dst->dec_ref_pic_marking_bit_size = slice_src->dec_ref_pic_marking_bit_size;
	dst->pic_order_cnt_bit_size       = slice_src->pic_order_cnt_bit_size;
	dst->slice_group_change_cycle     = slice_src->slice_group_change_cycle;

	/* reserved = 0 garantido pelo memset */

	/* V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC=0x01, mesmo valor em ambas */
	dst->flags = src->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
}

/* -----------------------------------------------------------------------
 * h264_translate_and_set_controls() — ponto de entrada público
 *
 * Traduz todas as structs internas para as modernas e as envia ao kernel.
 *
 * pred_weights é enviada como controle separado APENAS quando a macro
 * V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED indica necessidade, ou seja:
 *   - slice P/SP com pps.weighted_pred_flag=1, OU
 *   - slice B com pps.weighted_bipred_idc=1
 * Para os demais casos (I-frame, P sem weighted_pred, B sem explicit
 * bipred weights) enviar pred_weights desnecessariamente pode causar
 * EINVAL em alguns drivers.
 * ----------------------------------------------------------------------- */
int h264_translate_and_set_controls(
	int video_fd,
	int request_fd,
	const struct v4l2_ctrl_h264_sps_internal    		*sps_proj,
	const struct v4l2_ctrl_h264_pps_internal			*pps_proj,
	const struct v4l2_ctrl_h264_scaling_matrix_internal *matrix_proj,
	const struct v4l2_ctrl_h264_slice_params_internal   *slice_proj,
	const struct v4l2_ctrl_h264_decode_params_internal  *decode_proj)
{
	struct v4l2_ctrl_h264_sps            sps;
	struct v4l2_ctrl_h264_pps            pps;
	struct v4l2_ctrl_h264_scaling_matrix matrix;
	struct v4l2_ctrl_h264_pred_weights   pred_weights;
	struct v4l2_ctrl_h264_slice_params   slice;
	struct v4l2_ctrl_h264_decode_params  decode;
	int need_pred_weights;
	int rc;

	translate_sps(sps_proj, &sps);
	translate_pps(pps_proj, &pps);
	translate_scaling_matrix(matrix_proj, &matrix);
	translate_slice_params(slice_proj, &slice);
	translate_decode_params(decode_proj, slice_proj, &decode);

/*
	 * Sempre enviar PRED_WEIGHTS (mesmo se não for obrigatório) para
	 * sobrescrever dados residuais de requisições anteriores, já que
	 * MEDIA_REQUEST_IOC_REINIT é no-op no BSP T527.
	 */
	translate_pred_weights(slice_proj, &pred_weights);
	need_pred_weights = 1;

	/* --- DECODE --- */
	rc = v4l2_set_control(video_fd, request_fd,
			      V4L2_CID_STATELESS_H264_DECODE_PARAMS,
			      &decode, sizeof(decode));
	if (rc < 0) {
		request_log("h264_translate: DECODE FAILED\n");
		return -1;
	}

	/* --- SLICE --- */
	rc = v4l2_set_control(video_fd, request_fd,
			      V4L2_CID_STATELESS_H264_SLICE_PARAMS,
			      &slice, sizeof(slice));
	if (rc < 0) {
		request_log("h264_translate: SLICE FAILED\n");
		return -1;
	}

	/* --- PRED_WEIGHTS (condicional) --- */
	if (need_pred_weights) {
		rc = v4l2_set_control(video_fd, request_fd,
				      V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
				      &pred_weights, sizeof(pred_weights));
		if (rc < 0) {
			request_log("h264_translate: PRED_WEIGHTS FAILED\n");
			return -1;
		}
	}

	/* --- PPS --- */
	rc = v4l2_set_control(video_fd, request_fd,
			      V4L2_CID_STATELESS_H264_PPS,
			      &pps, sizeof(pps));
	if (rc < 0) {
		request_log("h264_translate: PPS FAILED\n");
		return -1;
	}

	/* --- SPS --- */
	rc = v4l2_set_control(video_fd, request_fd,
			      V4L2_CID_STATELESS_H264_SPS,
			      &sps, sizeof(sps));
	if (rc < 0) {
		request_log("h264_translate: SPS FAILED\n");
		return -1;
	}

	/* --- SCALING MATRIX --- */
	rc = v4l2_set_control(video_fd, request_fd,
			      V4L2_CID_STATELESS_H264_SCALING_MATRIX,
			      &matrix, sizeof(matrix));
	if (rc < 0) {
		request_log("h264_translate: SCALING_MATRIX FAILED\n");
		return -1;
	}

	return 0;
}
