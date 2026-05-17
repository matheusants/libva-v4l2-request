/*
 * H264 encode parameter translation for the sunxi-venc V4L2 M2M encoder.
 *
 * Part of the Path B open-source T527 H264 encoder (milestone M4): bridges
 * VAAPI encode parameter buffers to the standard V4L2_CID_MPEG_VIDEO_*
 * controls exposed by the sunxi-venc kernel driver.
 */

#ifndef _H264_ENC_H_
#define _H264_ENC_H_

#include <va/va_backend.h>

#include "context.h"

/*
 * Apply the latched VAAPI encode parameters to the sunxi-venc encoder fd as
 * V4L2 controls. When sequence_controls is true the stream-level controls
 * (bitrate, GOP, profile, level) are programmed too — do this once, before
 * STREAMON. The per-frame controls (QP, force-keyframe) are always applied.
 */
int h264_enc_set_controls(struct object_context *context, VAProfile profile,
			  bool sequence_controls);

#endif
