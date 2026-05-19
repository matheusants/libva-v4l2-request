/*
 * H264 encode parameter translation for the sunxi-venc V4L2 M2M encoder.
 *
 * Part of the Path B open-source T527 H264 encoder (milestone M4).
 */

#include "h264_enc.h"

#include <errno.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "utils.h"

/* Set one V4L2 control; missing controls are tolerated (logged, not fatal). */
static void enc_set_ctrl(int fd, unsigned int id, int value, const char *name)
{
	struct v4l2_control ctrl = { .id = id, .value = value };

	if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
		request_log("h264_enc: S_CTRL %s=%d failed: %s\n",
			    name, value, strerror(errno));
}

/* Map an H264 level_idc value to the V4L2 H264 level menu index. */
static int enc_level_menu(unsigned int level_idc)
{
	switch (level_idc) {
	case 10: return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
	case 11: return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
	case 12: return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
	case 13: return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
	case 20: return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
	case 21: return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
	case 22: return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
	case 30: return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
	case 31: return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
	case 32: return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
	case 40: return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
	case 41: return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
	case 42: return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
	default: return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
	}
}

/*
 * The VE bit-writer emits only baseline-syntax SPS, so encode is capped at
 * Main — High would need the scaling-matrix block the writer does not produce.
 */
static int enc_profile_menu(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264Main:
	case VAProfileH264High:
		return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
	case VAProfileH264ConstrainedBaseline:
	default:
		return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
	}
}

int h264_enc_set_controls(struct object_context *context, VAProfile profile,
			  bool sequence_controls)
{
	int fd = context->encoder_fd;
	unsigned int qp;

	if (fd < 0)
		return -1;

	/* Stream-level controls — program once, before STREAMON. */
	if (sequence_controls && context->enc_seq_valid) {
		VAEncSequenceParameterBufferH264 *seq = &context->enc_seq;
		unsigned int gop;

		/*
		 * Prefer the RC misc buffer's target bitrate; the sequence
		 * buffer only carries the VBR peak (often 2x the target).
		 */
		unsigned int bitrate = context->enc_rc_valid ?
			context->enc_rc_bitrate : seq->bits_per_second;

		request_log("h264_enc: seq peak_bps=%u rc_bps=%u idr=%u "
			    "intra=%u level=%u units_in_tick=%u time_scale=%u\n",
			    seq->bits_per_second,
			    context->enc_rc_valid ? context->enc_rc_bitrate : 0,
			    seq->intra_idr_period, seq->intra_period,
			    seq->level_idc, seq->num_units_in_tick,
			    seq->time_scale);

		if (bitrate > 0)
			enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE,
				     bitrate, "BITRATE");

		/* Frames between IDRs: prefer the IDR period. */
		gop = seq->intra_idr_period ? seq->intra_idr_period :
		      seq->intra_period;
		if (gop > 0)
			enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop,
				     "GOP_SIZE");

		enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			     enc_profile_menu(profile), "H264_PROFILE");

		if (seq->level_idc > 0)
			enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
				     enc_level_menu(seq->level_idc),
				     "H264_LEVEL");

		/*
		 * Frame rate drives the driver-side rate-control bit budget.
		 * H264 fps = time_scale / (2 * num_units_in_tick); feed it via
		 * S_PARM so RC budgets per-frame bits at the real rate (a wrong
		 * default overshoots the target bitrate proportionally).
		 */
		if (seq->num_units_in_tick > 0 && seq->time_scale > 0) {
			struct v4l2_streamparm parm = {
				.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			};

			parm.parm.output.timeperframe.numerator =
				2 * seq->num_units_in_tick;
			parm.parm.output.timeperframe.denominator =
				seq->time_scale;
			if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0)
				request_log("h264_enc: S_PARM fps failed: %s\n",
					    strerror(errno));
		}

		/*
		 * ffmpeg/VAAPI hands the encoder a 16-aligned coded size; the
		 * real visible rectangle lives in the SPS frame_cropping
		 * fields. Convey it as an OUTPUT crop selection so the driver
		 * emits matching SPS frame_cropping — without it the bottom
		 * padding macroblock rows decode as green.
		 */
		if (seq->frame_cropping_flag) {
			/* 4:2:0 progressive: CropUnitX = CropUnitY = 2. */
			struct v4l2_selection sel = {
				.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				.target = V4L2_SEL_TGT_CROP,
			};

			sel.r.left = 0;
			sel.r.top = 0;
			sel.r.width = context->picture_width -
				2 * (seq->frame_crop_left_offset +
				     seq->frame_crop_right_offset);
			sel.r.height = context->picture_height -
				2 * (seq->frame_crop_top_offset +
				     seq->frame_crop_bottom_offset);

			request_log("h264_enc: crop visible %ux%u "
				    "(coded %dx%d)\n", sel.r.width,
				    sel.r.height, context->picture_width,
				    context->picture_height);

			if (ioctl(fd, VIDIOC_S_SELECTION, &sel) < 0)
				request_log("h264_enc: S_SELECTION failed: "
					    "%s\n", strerror(errno));
		}
	}

	/* Per-frame controls. */
	if (!context->enc_pic_valid)
		return 0;

	/* pic_init_qp seeds both I and P frame QP. */
	qp = context->enc_pic.pic_init_qp;
	if (qp >= 1 && qp <= 51) {
		enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp,
			     "I_FRAME_QP");
		enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp,
			     "P_FRAME_QP");
	}

	/* Honour an explicitly requested IDR (the GOP control covers the
	 * periodic case; this catches scene-cut / mid-GOP keyframes). */
	if (context->enc_pic.pic_fields.bits.idr_pic_flag &&
	    context->enc_frame_num > 0)
		enc_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0,
			     "FORCE_KEY_FRAME");

	return 0;
}
