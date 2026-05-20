/*
 * v4l2_translation.h — Tradução structs H.264 internas → structs modernas
 *
 * O projeto usa structs "antigas" (Bootlin/staging) internamente.
 * O kernel T527/Cedrus 5.15 BSP espera as structs modernas definidas em
 * <linux/v4l2-controls.h> com os CIDs V4L2_CID_STATELESS_H264_*.
 *
 * Este módulo faz a tradução campo a campo e envia via v4l2_set_control().
 *
 * Diferenças críticas tratadas aqui:
 *
 *   SLICE (892→152 bytes):
 *     - Descarta: size, pred_weight_table, pic_parameter_set_id,
 *       frame_num, idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt*,
 *       dec_ref_pic_marking_bit_size, pic_order_cnt_bit_size,
 *       slice_group_change_cycle, slice_qs_delta.
 *     - ref_pic_list0/1: __u8[32] → struct v4l2_h264_reference[32]
 *       onde .index = old[i], .fields = V4L2_H264_FRAME_REF (0x3).
 *
 *   DECODE (496→560 bytes):
 *     - dpb_entry: 24→32 bytes, reordenação de campos + fields/reserved.
 *     - Descarta: num_slices, ref_pic_list_p0/b0/b1[].
 *     - Adiciona campos do slice interno: idr_pic_id, frame_num,
 *       pic_order_cnt_lsb, delta_pic_order_cnt*, dec_ref_pic_marking_bit_size,
 *       pic_order_cnt_bit_size, slice_group_change_cycle.
 *
 *   SPS (1048 bytes): layout idêntico — cópia direta, apenas CID diferente.
 *   PPS (12 bytes):   layout idêntico — cópia direta, apenas CID diferente.
 *   MATRIX (480 bytes): layout idêntico — cópia direta, apenas CID diferente.
 */

#ifndef _V4L2_TRANSLATION_H_
#define _V4L2_TRANSLATION_H_

#include "h264-ctrls.h"

/*
 * h264_translate_and_set_controls() — ponto de entrada principal.
 *
 * Recebe as cinco structs internas (_internal) já preenchidas por h264.c,
 * traduz cada uma para a struct moderna correspondente e envia ao kernel
 * via VIDIOC_S_EXT_CTRLS com os CIDs STATELESS corretos.
 *
 * O parâmetro @slice é necessário também para preencher campos do
 * decode_params moderno (idr_pic_id, frame_num, pic_order_cnt_lsb,
 * delta_pic_order_cnt*, etc.) que no formato antigo estavam no slice
 * header mas no moderno ficam no decode_params.
 *
 * @video_fd:   file descriptor do dispositivo V4L2
 * @request_fd: request fd (>= 0 para Request API, -1 para CUR_VAL)
 * @sps:        ponteiro para SPS interno preenchido
 * @pps:        ponteiro para PPS interno preenchido
 * @matrix:     ponteiro para scaling matrix interna preenchida
 * @slice:      ponteiro para slice params internos preenchidos
 * @decode:     ponteiro para decode params internos preenchidos
 *
 * Retorna 0 em sucesso, -1 em falha (errno preservado de v4l2_set_control).
 */
struct request_data;
int h264_translate_and_set_controls(
	struct request_data *driver_data,
	int video_fd,
	int request_fd,
	const struct v4l2_ctrl_h264_sps_internal    *sps,
	const struct v4l2_ctrl_h264_pps_internal    *pps,
	const struct v4l2_ctrl_h264_scaling_matrix_internal *matrix,
	const struct v4l2_ctrl_h264_slice_params_internal   *slice,
	const struct v4l2_ctrl_h264_decode_params_internal  *decode);

#endif /* _V4L2_TRANSLATION_H_ */
