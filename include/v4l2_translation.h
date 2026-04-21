#include <linux/types.h>
#include <linux/videodev2.h>

struct kernel_h264_decode_params {
    struct v4l2_h264_dpb_entry dpb[16];

    __u16 nal_ref_idc;
    __u16 frame_num;

    __s32 top_field_order_cnt;
    __s32 bottom_field_order_cnt;

    __u32 flags;
};