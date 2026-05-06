/* SPDX-License-Identifier: GPL-2.0 */
/*
 * H.264 stateless codec controls — structs internas do projeto.
 *
 * Estas structs são as versões "antigas" (Bootlin/staging) usadas
 * internamente pelo código VAAPI para acumular os parâmetros.
 * Foram renomeadas com sufixo _internal para evitar colisão com as
 * structs modernas definidas em <linux/v4l2-controls.h> (incluídas
 * via <linux/videodev2.h>), que são as que o kernel T527/Cedrus espera.
 *
 * A tradução antiga→moderna é feita em v4l2_translation.c antes do
 * VIDIOC_S_EXT_CTRLS.
 */
#pragma once

#ifndef _H264_CTRLS_H_
#define _H264_CTRLS_H_

//#include "v4l2-compat.h"

#include <linux/types.h>
#include <linux/videodev2.h>

/* Pixel format não-estável usado no projeto */
#define V4L2_PIX_FMT_H264_SLICE_RAW v4l2_fourcc('S', '2', '6', '4')

/* =========================
 * DEFINES MPEG (BSP antigo)
 * ========================= */
#define V4L2_CID_MPEG_VIDEO_H264_SPS 			(V4L2_CID_MPEG_BASE + 1000)
#define V4L2_CID_MPEG_VIDEO_H264_PPS 			(V4L2_CID_MPEG_BASE + 1001)
#define V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX (V4L2_CID_MPEG_BASE + 1002)
#define V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAMS 	(V4L2_CID_MPEG_BASE + 1003)
#define V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAMS 	(V4L2_CID_MPEG_BASE + 1004)

/* enum v4l2_ctrl_type type values (usados apenas internamente) */
#define V4L2_CTRL_TYPE_H264_SPS 			0x0110
#define V4L2_CTRL_TYPE_H264_PPS 			0x0111
#define V4L2_CTRL_TYPE_H264_SCALING_MATRIX 	0x0112
#define V4L2_CTRL_TYPE_H264_SLICE_PARAMS 	0x0113
#define V4L2_CTRL_TYPE_H264_DECODE_PARAMS 	0x0114

/* -----------------------------------------------------------------------
 * Flags SPS — idênticos aos do sistema, definidos aqui para referência
 * interna (os do sistema já vêm via v4l2-controls.h, mas como não
 * incluímos esse header diretamente aqui, redefinimos com guarda).
 * ----------------------------------------------------------------------- */
#ifndef V4L2_H264_SPS_CONSTRAINT_SET0_FLAG
#define V4L2_H264_SPS_CONSTRAINT_SET0_FLAG 0x01
#define V4L2_H264_SPS_CONSTRAINT_SET1_FLAG 0x02
#define V4L2_H264_SPS_CONSTRAINT_SET2_FLAG 0x04
#define V4L2_H264_SPS_CONSTRAINT_SET3_FLAG 0x08
#define V4L2_H264_SPS_CONSTRAINT_SET4_FLAG 0x10
#define V4L2_H264_SPS_CONSTRAINT_SET5_FLAG 0x20
#endif

#ifndef V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE
#define V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE 			0x01
#define V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS 	0x02
#define V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO 		0x04
#define V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED 	0x08
#define V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY 					0x10
#define V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD 			0x20
#define V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE 			0x40
#endif

/* -----------------------------------------------------------------------
 * Flags PPS
 * ----------------------------------------------------------------------- */
#ifndef V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE
#define V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE 						0x0001
#define V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT 	0x0002
#define V4L2_H264_PPS_FLAG_WEIGHTED_PRED 							0x0004
#define V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT 		0x0008
#define V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED 					0x0010
#define V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT 				0x0020
#define V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE 						0x0040
/* Nota: o flag PIC_SCALING_MATRIX_PRESENT (antigo 0x0080) é equivalente
 * ao SCALING_MATRIX_PRESENT moderno — mantemos o valor aqui para
 * compatibilidade com o código existente em h264.c */
#define V4L2_H264_PPS_FLAG_PIC_SCALING_MATRIX_PRESENT 				0x0080
#endif

/* -----------------------------------------------------------------------
 * Slice type e flags de slice — idênticos ao sistema
 * ----------------------------------------------------------------------- */
#ifndef V4L2_H264_SLICE_TYPE_P
#define V4L2_H264_SLICE_TYPE_P 	0
#define V4L2_H264_SLICE_TYPE_B 	1
#define V4L2_H264_SLICE_TYPE_I 	2
#define V4L2_H264_SLICE_TYPE_SP 3
#define V4L2_H264_SLICE_TYPE_SI 4
#endif

/* Flags de slice — versão antiga (usados em h264.c) */
#define V4L2_H264_SLICE_FLAG_FIELD_PIC 				0x01
#define V4L2_H264_SLICE_FLAG_BOTTOM_FIELD 			0x02
#ifndef V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED
#define V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED 0x04
#define V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH 			0x08
#endif

/* -----------------------------------------------------------------------
 * DPB flags — idênticos ao sistema
 * ----------------------------------------------------------------------- */
#ifndef V4L2_H264_DPB_ENTRY_FLAG_VALID
#define V4L2_H264_DPB_ENTRY_FLAG_VALID 		0x01
#define V4L2_H264_DPB_ENTRY_FLAG_ACTIVE 	0x02
#define V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM 	0x04
#endif

/* -----------------------------------------------------------------------
 * Decode params flag
 * ----------------------------------------------------------------------- */
#ifndef V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC
#define V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC 0x01
#endif

/* =======================================================================
 * STRUCTS INTERNAS (_internal)
 *
 * Estas são as estruturas no formato "antigo" (Bootlin/staging) que o
 * código VAAPI (h264.c) preenche. Sufixo _internal para não colidir com
 * as structs modernas do kernel (v4l2_ctrl_h264_sps, etc.) que vêm
 * de <linux/v4l2-controls.h>.
 * ======================================================================= */

/**
 * struct v4l2_ctrl_h264_sps_internal - SPS interno (antigo)
 * Tamanho: 1048 bytes. Idêntico à struct moderna — sem necessidade de
 * tradução de campos, apenas de nome para evitar colisão.
 */
struct v4l2_ctrl_h264_sps_internal
{
	__u8 profile_idc;
	__u8 constraint_set_flags;
	__u8 level_idc;
	__u8 seq_parameter_set_id;
	__u8 chroma_format_idc;
	__u8 bit_depth_luma_minus8;
	__u8 bit_depth_chroma_minus8;
	__u8 log2_max_frame_num_minus4;
	__u8 pic_order_cnt_type;
	__u8 log2_max_pic_order_cnt_lsb_minus4;
	__u8 max_num_ref_frames;
	__u8 num_ref_frames_in_pic_order_cnt_cycle;
	__s32 offset_for_ref_frame[255];
	__s32 offset_for_non_ref_pic;
	__s32 offset_for_top_to_bottom_field;
	__u16 pic_width_in_mbs_minus1;
	__u16 pic_height_in_map_units_minus1;
	__u32 flags;
};

/**
 * struct v4l2_ctrl_h264_pps_internal - PPS interno (antigo)
 * Tamanho: 12 bytes. Idêntico à struct moderna.
 */
struct v4l2_ctrl_h264_pps_internal
{
	__u8 pic_parameter_set_id;
	__u8 seq_parameter_set_id;
	__u8 num_slice_groups_minus1;
	__u8 num_ref_idx_l0_default_active_minus1;
	__u8 num_ref_idx_l1_default_active_minus1;
	__u8 weighted_bipred_idc;
	__s8 pic_init_qp_minus26;
	__s8 pic_init_qs_minus26;
	__s8 chroma_qp_index_offset;
	__s8 second_chroma_qp_index_offset;
	__u16 flags;
};

/**
 * struct v4l2_ctrl_h264_scaling_matrix_internal - Scaling matrix interna
 * Tamanho: 480 bytes. Idêntico à struct moderna.
 */
struct v4l2_ctrl_h264_scaling_matrix_internal
{
	__u8 scaling_list_4x4[6][16];
	__u8 scaling_list_8x8[6][64];
};

/**
 * struct v4l2_h264_weight_factors - Fatores de peso (usados em pred_weight)
 * Compartilhado entre versão antiga e nova (estrutura idêntica).
 */
#ifndef _V4L2_H264_WEIGHT_FACTORS_DEFINED
#define _V4L2_H264_WEIGHT_FACTORS_DEFINED
/* Evitar redefinição se v4l2-controls.h já foi incluído antes */
#endif
struct v4l2_h264_weight_factors_internal
{
	__s16 luma_weight[32];
	__s16 luma_offset[32];
	__s16 chroma_weight[32][2];
	__s16 chroma_offset[32][2];
};

/**
 * struct v4l2_h264_pred_weight_table_internal - Tabela de pesos preditivos
 */
struct v4l2_h264_pred_weight_table_internal
{
	__u16 luma_log2_weight_denom;
	__u16 chroma_log2_weight_denom;
	struct v4l2_h264_weight_factors_internal weight_factors[2];
};

/**
 * struct v4l2_ctrl_h264_slice_params_internal - Slice params interno (antigo)
 * Tamanho: 892 bytes.
 *
 * DIFERENÇAS em relação à struct moderna (152 bytes):
 * - Tem campos extras: size, pred_weight_table, delta_pic_order_cnt*,
 *   dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *   slice_group_change_cycle, pic_parameter_set_id, frame_num, etc.
 * - ref_pic_list0/1 são __u8[32] (índices DPB diretos)
 * - Na struct moderna, ref_pic_list0/1 são struct v4l2_h264_reference[32]
 *   com campos {__u8 fields, __u8 index}
 */
struct v4l2_ctrl_h264_slice_params_internal
{
	/* Size in bytes, including header */
	__u32 size;
	/* Offset in bits to slice_data() from the beginning of this slice. */
	__u32 header_bit_size;

	__u16 first_mb_in_slice;
	__u8 slice_type;
	__u8 pic_parameter_set_id;
	__u8 colour_plane_id;
	__u8 redundant_pic_cnt;
	__u16 frame_num;
	__u16 idr_pic_id;
	__u16 pic_order_cnt_lsb;
	__s32 delta_pic_order_cnt_bottom;
	__s32 delta_pic_order_cnt0;
	__s32 delta_pic_order_cnt1;

	struct v4l2_h264_pred_weight_table_internal pred_weight_table;
	/* Size in bits of dec_ref_pic_marking() syntax element. */
	__u32 dec_ref_pic_marking_bit_size;
	/* Size in bits of pic order count syntax. */
	__u32 pic_order_cnt_bit_size;

	__u8 cabac_init_idc;
	__s8 slice_qp_delta;
	__s8 slice_qs_delta;
	__u8 disable_deblocking_filter_idc;
	__s8 slice_alpha_c0_offset_div2;
	__s8 slice_beta_offset_div2;
	__u8 num_ref_idx_l0_active_minus1;
	__u8 num_ref_idx_l1_active_minus1;
	__u32 slice_group_change_cycle;

	/*
	 * Índices no DPB — __u8[32], cada byte é o índice DPB direto.
	 * Na struct moderna, estes são struct v4l2_h264_reference[32]
	 * com { __u8 fields, __u8 index }.
	 */
	__u8 ref_pic_list0[32];
	__u8 ref_pic_list1[32];

	__u32 flags;

	__u8 ref_pic_list0_fields[32];
	__u8 ref_pic_list1_fields[32];
	
};

/**
 * struct v4l2_h264_dpb_entry_internal - DPB entry interna (antiga, 24 bytes)
 *
 * DIFERENÇAS em relação à struct moderna (32 bytes):
 * - Antiga:  {u64 ref_ts, u16 frame_num, u16 pic_num, s32 top, s32 bot, u32 flags}
 * - Moderna: {u64 ref_ts, u32 pic_num,   u16 frame_num, u8 fields, u8 reserved[5],
 *             s32 top, s32 bot, u32 flags}
 * A ordem de frame_num/pic_num está invertida e há campo fields+reserved adicionais.
 */
struct v4l2_h264_dpb_entry_internal
{
	__u64 reference_ts;
	__u16 frame_num;
	__u16 pic_num;
	/* Note that field is indicated by v4l2_buffer.field */
	__s32 top_field_order_cnt;
	__s32 bottom_field_order_cnt;
	__u32 flags; /* V4L2_H264_DPB_ENTRY_FLAG_* */
};

/**
 * struct v4l2_ctrl_h264_decode_params_internal - Decode params interno (antigo)
 * Tamanho: 496 bytes.
 *
 * DIFERENÇAS em relação à struct moderna (624 bytes):
 * - Usa dpb_entry_internal (24 bytes) vs dpb_entry moderno (32 bytes):
 *   16×24=384 vs 16×32=512 → diferença de 128 bytes no dpb sozinho.
 * - Tem campos extras: num_slices, ref_pic_list_p0/b0/b1[32].
 * - Não tem: idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt*,
 *   dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *   slice_group_change_cycle, reserved.
 */
struct v4l2_ctrl_h264_decode_params_internal
{
	struct v4l2_h264_dpb_entry_internal dpb[16];
	__u16 num_slices;
	__u16 nal_ref_idc;
	__u8 ref_pic_list_p0[32];
	__u8 ref_pic_list_b0[32];
	__u8 ref_pic_list_b1[32];
	__s32 top_field_order_cnt;
	__s32 bottom_field_order_cnt;
	__u32 flags; /* V4L2_H264_DECODE_PARAM_FLAG_* */
};

#endif /* _H264_CTRLS_H_ */
