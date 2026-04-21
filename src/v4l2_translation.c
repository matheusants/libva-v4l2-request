

#include "h264-ctrls.h"
#include "v4l2.h"
#include "request.h"
#include "v4l2_translation.h"

#include <string.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include <linux/types.h>

void translate_sps(
    struct v4l2_ctrl_h264_sps_PROJECT *in,
    struct v4l2_ctrl_h264_sps *out)
{
    memset(out, 0, sizeof(*out));

    out->profile_idc = in->profile_idc;
    out->constraint_set_flags = in->constraint_set_flags;
    out->level_idc = in->level_idc;

    out->seq_parameter_set_id = in->seq_parameter_set_id;
    out->chroma_format_idc = in->chroma_format_idc;

    out->bit_depth_luma_minus8 = in->bit_depth_luma_minus8;
    out->bit_depth_chroma_minus8 = in->bit_depth_chroma_minus8;

    out->log2_max_frame_num_minus4 = in->log2_max_frame_num_minus4;
    out->pic_order_cnt_type = in->pic_order_cnt_type;
    out->log2_max_pic_order_cnt_lsb_minus4 =
        in->log2_max_pic_order_cnt_lsb_minus4;

    out->max_num_ref_frames = in->max_num_ref_frames;
    out->num_ref_frames_in_pic_order_cnt_cycle =
        in->num_ref_frames_in_pic_order_cnt_cycle;

    memcpy(out->offset_for_ref_frame,
           in->offset_for_ref_frame,
           sizeof(out->offset_for_ref_frame));

    out->offset_for_non_ref_pic = in->offset_for_non_ref_pic;
    out->offset_for_top_to_bottom_field =
        in->offset_for_top_to_bottom_field;

    out->pic_width_in_mbs_minus1 = in->pic_width_in_mbs_minus1;
    out->pic_height_in_map_units_minus1 =
        in->pic_height_in_map_units_minus1;

    out->flags = in->flags;
}

void translate_pps(
    struct v4l2_ctrl_h264_pps_PROJECT *in,
    struct v4l2_ctrl_h264_pps *out)
{
    memset(out, 0, sizeof(*out));

    out->pic_parameter_set_id = in->pic_parameter_set_id;
    out->seq_parameter_set_id = in->seq_parameter_set_id;

    out->num_slice_groups_minus1 = in->num_slice_groups_minus1;
    out->num_ref_idx_l0_default_active_minus1 =
        in->num_ref_idx_l0_default_active_minus1;
    out->num_ref_idx_l1_default_active_minus1 =
        in->num_ref_idx_l1_default_active_minus1;

    out->weighted_bipred_idc = in->weighted_bipred_idc;

    out->pic_init_qp_minus26 = in->pic_init_qp_minus26;
    out->pic_init_qs_minus26 = in->pic_init_qs_minus26;

    out->chroma_qp_index_offset = in->chroma_qp_index_offset;
    out->second_chroma_qp_index_offset =
        in->second_chroma_qp_index_offset;

    out->flags = in->flags;
}

void translate_matrix(
    struct v4l2_ctrl_h264_scaling_matrix_PROJECT *in,
    struct v4l2_ctrl_h264_scaling_matrix *out)
{
    memcpy(out, in, sizeof(*out));
}

void translate_slice(
    struct v4l2_ctrl_h264_slice_params_PROJECT *in,
    struct v4l2_ctrl_h264_slice_params *out)
{
    memset(out, 0, sizeof(*out));

    out->size = in->size;
    out->header_bit_size = in->header_bit_size;

    out->first_mb_in_slice = in->first_mb_in_slice;
    out->slice_type = in->slice_type;

    out->pic_parameter_set_id = in->pic_parameter_set_id;
    out->colour_plane_id = in->colour_plane_id;
    out->redundant_pic_cnt = in->redundant_pic_cnt;

    out->frame_num = in->frame_num;
    out->idr_pic_id = in->idr_pic_id;
    out->pic_order_cnt_lsb = in->pic_order_cnt_lsb;

    out->delta_pic_order_cnt_bottom = in->delta_pic_order_cnt_bottom;
    out->delta_pic_order_cnt0 = in->delta_pic_order_cnt0;
    out->delta_pic_order_cnt1 = in->delta_pic_order_cnt1;

    out->cabac_init_idc = in->cabac_init_idc;
    out->slice_qp_delta = in->slice_qp_delta;
    out->slice_qs_delta = in->slice_qs_delta;

    out->disable_deblocking_filter_idc =
        in->disable_deblocking_filter_idc;

    out->slice_alpha_c0_offset_div2 =
        in->slice_alpha_c0_offset_div2;
    out->slice_beta_offset_div2 =
        in->slice_beta_offset_div2;

    out->num_ref_idx_l0_active_minus1 =
        in->num_ref_idx_l0_active_minus1;
    out->num_ref_idx_l1_active_minus1 =
        in->num_ref_idx_l1_active_minus1;

    memcpy(out->ref_pic_list0,
           in->ref_pic_list0,
           sizeof(out->ref_pic_list0));

    memcpy(out->ref_pic_list1,
           in->ref_pic_list1,
           sizeof(out->ref_pic_list1));

    out->flags = in->flags;
}

/* Tradução da tabela de pesos (extraída do SliceParams do projeto) */
static void translate_pred_weights(struct v4l2_ctrl_h264_pred_weights *dst,
                                   const struct v4l2_h264_pred_weight_table_PROJECT *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->luma_log2_weight_denom = src->luma_log2_weight_denom;
    dst->chroma_log2_weight_denom = src->chroma_log2_weight_denom;
    memcpy(&dst->weight_factors, &src->weight_factors, sizeof(dst->weight_factors));
}

/* Tradução do DPB (Ajustando tamanhos de pic_num e campos reserved) */
static void translate_dpb_entry(struct v4l2_h264_dpb_entry *dst,
                               const struct v4l2_h264_dpb_entry_PROJECT *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->reference_ts = src->reference_ts;
    dst->pic_num = (uint32_t)src->pic_num; // Extensão para u32
    dst->frame_num = src->frame_num;
    dst->fields = 0x3; // Assume progressivo (Top + Bottom) para o T527
    dst->top_field_order_cnt = src->top_field_order_cnt;
    dst->bottom_field_order_cnt = src->bottom_field_order_cnt;
    dst->flags = src->flags;
}

/* Tradução de Decode Params (Movendo campos que o kernel espera aqui) */
static void translate_decode_params(struct v4l2_ctrl_h264_decode_params *dst,
                                    const struct v4l2_ctrl_h264_decode_params_PROJECT *src_dec,
                                    const struct v4l2_ctrl_h264_slice_params_PROJECT *src_slice)
{
    memset(dst, 0, sizeof(*dst));
    for (int i = 0; i < 16; i++) {
        translate_dpb_entry(&dst->dpb[i], &src_dec->dpb[i]);
    }
    dst->nal_ref_idc = src_dec->nal_ref_idc;
    dst->flags = src_dec->flags;
    
    // Campos migrados do Slice para o Decode Params no kernel estável
    dst->frame_num = src_slice->frame_num;
    dst->idr_pic_id = src_slice->idr_pic_id;
    dst->pic_order_cnt_lsb = src_slice->pic_order_cnt_lsb;
    dst->top_field_order_cnt = src_dec->top_field_order_cnt;
    dst->bottom_field_order_cnt = src_dec->bottom_field_order_cnt;
    dst->delta_pic_order_cnt_bottom = src_slice->delta_pic_order_cnt_bottom;
    dst->delta_pic_order_cnt0 = src_slice->delta_pic_order_cnt0;
    dst->delta_pic_order_cnt1 = src_slice->delta_pic_order_cnt1;
    dst->dec_ref_pic_marking_bit_size = src_slice->dec_ref_pic_marking_bit_size;
    dst->pic_order_cnt_bit_size = src_slice->pic_order_cnt_bit_size;
    dst->slice_group_change_cycle = src_slice->slice_group_change_cycle;
}