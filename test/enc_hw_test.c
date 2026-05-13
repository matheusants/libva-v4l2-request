/*
 * enc_hw_test.c — minimal VE H264 encoder smoke test for T527
 *
 * Directly programs AVC encoder registers via /dev/cedar_dev.
 * Uses /dev/dma_heap/system for DMA-coherent buffers.
 * Requires: sunxi-ve.ko loaded (patched to probe "allwinner,sunxi-cedar-ve-enc")
 *
 * Build: gcc -O2 -o enc_hw_test enc_hw_test.c
 * Run:   sudo ./enc_hw_test [width height] > out.264
 *        ffplay out.264
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>

/* ---- cedar_dev ioctls (from bsp/drivers/ve/cedar-ve/cedar_ve.h) ---- */

enum IOCTL_CMD {
    IOCTL_UNKNOWN        = 0x100,
    IOCTL_GET_ENV_INFO,
    IOCTL_WAIT_VE_DE,
    IOCTL_WAIT_VE_EN,
    IOCTL_RESET_VE,
    IOCTL_ENABLE_VE,
    IOCTL_DISABLE_VE,
    IOCTL_SET_VE_FREQ,
    IOCTL_CONFIG_AVS2   = 0x200,
    IOCTL_GETVALUE_AVS2,
    IOCTL_PAUSE_AVS2,
    IOCTL_START_AVS2,
    IOCTL_RESET_AVS2,
    IOCTL_ADJUST_AVS2,
    IOCTL_ENGINE_REQ,
    IOCTL_ENGINE_REL,
    IOCTL_ENGINE_CHECK_DELAY,
    IOCTL_GET_IC_VER,
    IOCTL_ADJUST_AVS2_ABS,
    IOCTL_FLUSH_CACHE,
    IOCTL_SET_REFCOUNT,
    IOCTL_FLUSH_CACHE_ALL,
    IOCTL_TEST_VERSION,
    IOCTL_GET_LOCK       = 0x310,
    IOCTL_RELEASE_LOCK,
    IOCTL_SET_VOL        = 0x400,
    IOCTL_WAIT_JPEG_DEC  = 0x500,
    IOCTL_GET_REFCOUNT,
    IOCTL_GET_IOMMU_ADDR,
    IOCTL_FREE_IOMMU_ADDR,
    IOCTL_MAP_DMA_BUF,
    IOCTL_UNMAP_DMA_BUF,
    IOCTL_FLUSH_CACHE_RANGE,
};

#define VE_LOCK_VDEC  0x01
#define VE_LOCK_VENC  0x02

struct dma_buf_param {
    int          fd;
    unsigned int phy_addr;
};

struct cedarv_env_infomation {
    unsigned int phymem_start;
    int          phymem_total_size;
    uint64_t     address_macc;
};

struct cache_range {
    uint64_t start;
    uint64_t end;
};

/* ---- VE register offsets ---- */
#define VE_CTRL              0x000
#define VE_VERSION           0x0f0

/* ISP (input preprocessor) */
#define VE_ISP_INPUT_SIZE    0xa00
#define VE_ISP_INPUT_STRIDE  0xa04
#define VE_ISP_CTRL          0xa08
#define VE_ISP_INPUT_LUMA    0xa78
#define VE_ISP_INPUT_CHROMA  0xa7c

/* AVC encoder */
#define VE_AVC_PARAM         0xb04
#define VE_AVC_QP            0xb08
#define VE_AVC_MOTION_EST    0xb10
#define VE_AVC_CTRL          0xb14
#define VE_AVC_TRIGGER       0xb18
#define VE_AVC_STATUS        0xb1c
#define VE_AVC_BASIC_BITS    0xb20
#define VE_AVC_UNK_BUF      0xb60
#define VE_AVC_VLE_ADDR      0xb80
#define VE_AVC_VLE_END       0xb84
#define VE_AVC_VLE_OFFSET    0xb88
#define VE_AVC_VLE_MAX       0xb8c
#define VE_AVC_VLE_LENGTH    0xb90
#define VE_AVC_REF_LUMA      0xba0
#define VE_AVC_REF_CHROMA    0xba4
#define VE_AVC_REC_LUMA      0xbb0
#define VE_AVC_REC_CHROMA    0xbb4
#define VE_AVC_REF_SLUMA     0xbb8
#define VE_AVC_REC_SLUMA     0xbbc
#define VE_AVC_MB_INFO       0xbc0

#define VE_ENGINE_AVC        0xb

/* ---- helpers ---- */
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define MB(x)      ((x)/16)

static volatile uint32_t *ve_regs = NULL;

static inline void writel_ve(uint32_t val, uint32_t off) {
    ve_regs[off/4] = val;
}
static inline uint32_t readl_ve(uint32_t off) {
    return ve_regs[off/4];
}

/* ---- buffer ---- */
typedef struct {
    int      dmabuf_fd;
    void    *virt;
    uint32_t iommu_addr;
    size_t   size;
} DmaBuf;

static int heap_fd = -1;
static int cedar_fd = -1;

static int dmabuf_alloc(DmaBuf *b, size_t size)
{
    struct dma_heap_allocation_data data = {
        .len        = size,
        .fd_flags   = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        return -1;
    }
    b->dmabuf_fd = data.fd;
    b->size = size;

    b->virt = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, b->dmabuf_fd, 0);
    if (b->virt == MAP_FAILED) { perror("mmap dmabuf"); close(b->dmabuf_fd); return -1; }

    struct dma_buf_param p = { .fd = b->dmabuf_fd };
    if (ioctl(cedar_fd, IOCTL_MAP_DMA_BUF, &p) < 0) {
        perror("IOCTL_MAP_DMA_BUF");
        munmap(b->virt, size);
        close(b->dmabuf_fd);
        return -1;
    }
    b->iommu_addr = p.phy_addr;
    memset(b->virt, 0, size);
    return 0;
}

static void dmabuf_flush(DmaBuf *b)
{
    struct cache_range r = { (uint64_t)(uintptr_t)b->virt,
                             (uint64_t)(uintptr_t)b->virt + b->size };
    ioctl(cedar_fd, IOCTL_FLUSH_CACHE_RANGE, &r);
}

static void dmabuf_free(DmaBuf *b)
{
    ioctl(cedar_fd, IOCTL_UNMAP_DMA_BUF, &(struct dma_buf_param){ .fd = b->dmabuf_fd });
    munmap(b->virt, b->size);
    close(b->dmabuf_fd);
}

/* ---- HW bit writer ---- */
static void put_bits(int n, uint32_t val)
{
    writel_ve(val, VE_AVC_BASIC_BITS);
    writel_ve(0x1 | ((n & 0x1f) << 8), VE_AVC_TRIGGER);
}

static void put_ue(uint32_t val)
{
    val++;
    int bits = 0;
    uint32_t tmp = val;
    while (tmp >>= 1) bits++;
    if (bits) put_bits(bits, 0);
    put_bits(bits + 1, val);
}

static void put_se(int32_t val)
{
    put_ue(val <= 0 ? -2*val : 2*val - 1);
}

static void nal_start(int nal_ref_idc, int nal_unit_type)
{
    uint32_t saved = readl_ve(VE_AVC_PARAM);
    writel_ve(saved | (1u << 31), VE_AVC_PARAM);   /* disable emulation prevention */
    put_bits(24, 0);                                 /* 00 00 00 */
    put_bits(16, 0x100 | (nal_ref_idc << 5) | nal_unit_type);
    writel_ve(saved, VE_AVC_PARAM);
}

static void write_sps(int width, int height)
{
    int mb_w = MB(ALIGN(width, 16));
    int mb_h = MB(ALIGN(height, 16));

    nal_start(3, 7);                   /* SPS, nal_ref_idc=3 */
    put_bits(8, 66);                   /* profile_idc = Baseline */
    put_bits(8, 0xC0);                 /* constraint flags */
    put_bits(8, 30);                   /* level_idc = 3.0 */
    put_ue(0);                         /* seq_parameter_set_id = 0 */
    put_ue(0);                         /* log2_max_frame_num_minus4 = 0 */
    put_ue(0);                         /* pic_order_cnt_type = 0 */
    put_ue(0);                         /* log2_max_pic_order_cnt_lsb_minus4 = 0 */
    put_ue(0);                         /* max_num_ref_frames = 1 → encode as 0 */
    put_bits(1, 0);                    /* gaps_in_frame_num_value_allowed_flag */
    put_ue(mb_w - 1);                  /* pic_width_in_mbs_minus1 */
    put_ue(mb_h - 1);                  /* pic_height_in_map_units_minus1 */
    put_bits(1, 1);                    /* frame_mbs_only_flag */
    put_bits(1, 0);                    /* direct_8x8_inference_flag */
    /* crop */
    if ((width % 16) || (height % 16)) {
        put_bits(1, 1);
        put_ue(0); put_ue((ALIGN(width,16)-width)/2);
        put_ue(0); put_ue((ALIGN(height,16)-height)/2);
    } else {
        put_bits(1, 0);
    }
    put_bits(1, 0);                    /* vui_parameters_present_flag */
}

static void write_pps(void)
{
    nal_start(3, 8);                   /* PPS */
    put_ue(0);                         /* pic_parameter_set_id */
    put_ue(0);                         /* seq_parameter_set_id */
    put_bits(1, 0);                    /* entropy_coding_mode_flag = CAVLC */
    put_bits(1, 0);                    /* bottom_field_pic_order_in_frame_present_flag */
    put_ue(0);                         /* num_slice_groups_minus1 */
    put_ue(0);                         /* num_ref_idx_l0_default_active_minus1 */
    put_ue(0);                         /* num_ref_idx_l1_default_active_minus1 */
    put_bits(1, 0);                    /* weighted_pred_flag */
    put_bits(2, 0);                    /* weighted_bipred_idc */
    put_se(0);                         /* pic_init_qp_minus26 */
    put_se(0);                         /* pic_init_qs_minus26 */
    put_se(0);                         /* chroma_qp_index_offset */
    put_bits(1, 1);                    /* deblocking_filter_control_present_flag */
    put_bits(1, 0);                    /* constrained_intra_pred_flag */
    put_bits(1, 0);                    /* redundant_pic_cnt_present_flag */
}

static void write_slice_header(int is_idr, int frame_num, int qp)
{
    int nal_ref = is_idr ? 3 : 2;
    int nal_type = is_idr ? 5 : 1;
    nal_start(nal_ref, nal_type);
    put_ue(0);                          /* first_mb_in_slice */
    put_ue(is_idr ? 7 : 5);            /* slice_type: I=7, P=5 */
    put_ue(0);                          /* pic_parameter_set_id */
    put_bits(4, frame_num & 0xf);       /* frame_num (4 bits for log2_max=4) */
    if (is_idr) put_ue(0);             /* idr_pic_id */
    put_bits(4, 0);                     /* pic_order_cnt_lsb */
    if (!is_idr) {
        put_bits(1, 0);                 /* num_ref_idx_active_override_flag */
        /* ref_pic_list_modification: not present */
        put_bits(1, 0);
        /* dec_ref_pic_marking: not adaptive */
        put_bits(1, 0);
    }
    if (is_idr) {
        put_bits(1, 0);                 /* no_output_of_prior_pics_flag */
        put_bits(1, 0);                 /* long_term_reference_flag */
    }
    put_se(qp - 26);                    /* slice_qp_delta */
    put_bits(1, 1);                     /* disable_deblocking_filter_idc=0 → cabac flag=0 */
}

/* ---- encode one frame ---- */
static int encode_frame(
    int w, int h, int qp, int is_idr, int frame_num,
    DmaBuf *src_luma, DmaBuf *src_chroma,
    DmaBuf *ref_luma, DmaBuf *ref_chroma, DmaBuf *ref_sluma,
    DmaBuf *rec_luma, DmaBuf *rec_chroma, DmaBuf *rec_sluma,
    DmaBuf *mb_info,  DmaBuf *unk_buf,
    DmaBuf *vle_out,
    int write_headers)
{
    int mb_w      = MB(ALIGN(w, 16));
    int mb_h      = MB(ALIGN(h, 16));
    int mb_stride = mb_w;

    /* Select AVC engine */
    writel_ve(0x00130000 | VE_ENGINE_AVC, VE_CTRL);

    /* Clear bitstream offset */
    writel_ve(0, VE_AVC_VLE_OFFSET);

    /* AVC param: I-frame (no P-frame bit), CAVLC */
    uint32_t avc_param = is_idr ? 0 : (1u << 4);
    writel_ve(avc_param, VE_AVC_PARAM);

    /* Write SPS/PPS/slice headers via HW bit writer */
    if (write_headers) {
        write_sps(w, h);
        write_pps();
    }
    write_slice_header(is_idr, frame_num, qp);

    /* Output VLE buffer */
    writel_ve(vle_out->iommu_addr,                    VE_AVC_VLE_ADDR);
    writel_ve(vle_out->iommu_addr + vle_out->size - 1, VE_AVC_VLE_END);
    writel_ve(vle_out->size * 8,                      VE_AVC_VLE_MAX);

    /* ISP input */
    writel_ve((mb_w << 16) | mb_h,  VE_ISP_INPUT_SIZE);
    writel_ve(mb_stride << 16,       VE_ISP_INPUT_STRIDE);
    writel_ve(0 << 29,               VE_ISP_CTRL);      /* NV12 */
    writel_ve(src_luma->iommu_addr,  VE_ISP_INPUT_LUMA);
    writel_ve(src_chroma->iommu_addr,VE_ISP_INPUT_CHROMA);

    /* QP */
    writel_ve((4u << 16) | ((uint32_t)qp << 8) | (uint32_t)qp, VE_AVC_QP);

    /* Motion estimation */
    writel_ve(0x00000104, VE_AVC_MOTION_EST);

    /* Ref/rec buffers */
    writel_ve(ref_luma->iommu_addr,   VE_AVC_REF_LUMA);
    writel_ve(ref_chroma->iommu_addr, VE_AVC_REF_CHROMA);
    writel_ve(ref_sluma->iommu_addr,  VE_AVC_REF_SLUMA);
    writel_ve(rec_luma->iommu_addr,   VE_AVC_REC_LUMA);
    writel_ve(rec_chroma->iommu_addr, VE_AVC_REC_CHROMA);
    writel_ve(rec_sluma->iommu_addr,  VE_AVC_REC_SLUMA);

    /* MB info + unknown buf */
    writel_ve(mb_info->iommu_addr, VE_AVC_MB_INFO);
    writel_ve(unk_buf->iommu_addr, VE_AVC_UNK_BUF);

    /* Cache flush */
    dmabuf_flush(src_luma);
    dmabuf_flush(src_chroma);
    dmabuf_flush(vle_out);

    /* Disable interrupts (poll mode), clear status */
    writel_ve(0x0, VE_AVC_CTRL);
    writel_ve(0x7, VE_AVC_STATUS);

    /* Trigger encode */
    writel_ve(0x8, VE_AVC_TRIGGER);

    /* Poll completion (sunxi-ve.ko does same in poll mode) */
    int timeout = 1000000;
    uint32_t status;
    do {
        status = readl_ve(VE_AVC_STATUS);
        if (status & 0x3) break;
    } while (--timeout > 0);

    if (timeout <= 0) {
        fprintf(stderr, "encode timeout! status=0x%x\n", status);
        return -1;
    }
    if (status & 0x4) {
        fprintf(stderr, "encode error! status=0x%x\n", status);
        return -1;
    }

    /* Clear status */
    writel_ve(status, VE_AVC_STATUS);

    uint32_t bits = readl_ve(VE_AVC_VLE_LENGTH);
    return (int)(bits / 8);
}

int main(int argc, char **argv)
{
    int width  = (argc >= 3) ? atoi(argv[1]) : 320;
    int height = (argc >= 3) ? atoi(argv[2]) : 240;
    int frames = (argc >= 4) ? atoi(argv[3]) : 10;
    int qp     = 28;

    int mb_w   = MB(ALIGN(width,  16));
    int mb_h   = MB(ALIGN(height, 16));
    int al_w   = ALIGN(mb_w * 16, 32);
    int al_h   = ALIGN(mb_h * 16, 32);

    size_t luma_sz    = al_w * al_h;
    size_t chroma_sz  = luma_sz / 2;
    size_t sluma_sz   = luma_sz / 4;
    size_t mbinfo_sz  = (size_t)ALIGN(mb_w, 4) * mb_h * 32;
    size_t unk_sz     = (size_t)ALIGN(mb_w, 4) * mb_h * 8;
    size_t vle_sz     = (size_t)(mb_w * mb_h * 384);  /* generous */

    fprintf(stderr, "enc_hw_test: %dx%d, %d frames, QP=%d\n",
            width, height, frames, qp);

    /* Open /dev/cedar_dev */
    cedar_fd = open("/dev/cedar_dev", O_RDWR);
    if (cedar_fd < 0) {
        perror("open /dev/cedar_dev");
        fprintf(stderr, "Ensure sunxi-ve.ko is loaded (modprobe sunxi_ve)\n");
        return 1;
    }

    /* Lock VE for encode */
    ioctl(cedar_fd, IOCTL_GET_LOCK, VE_LOCK_VENC);
    ioctl(cedar_fd, IOCTL_ENABLE_VE, 0);

    /* Open dma_heap */
    heap_fd = open("/dev/dma_heap/system", O_RDONLY);
    if (heap_fd < 0) { perror("open /dev/dma_heap/system"); goto cleanup; }

    /* Map VE registers via cedar_dev */
    struct cedarv_env_infomation env = {};
    if (ioctl(cedar_fd, IOCTL_GET_ENV_INFO, &env) < 0) {
        perror("IOCTL_GET_ENV_INFO");
        goto cleanup;
    }
    /* Map regs — cedar_dev exposes them as mmap offset 0 */
    ve_regs = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, cedar_fd, 0);
    if (ve_regs == MAP_FAILED) {
        perror("mmap cedar_dev regs");
        goto cleanup;
    }

    uint32_t ver = readl_ve(VE_VERSION) >> 16;
    fprintf(stderr, "VE version: 0x%04x\n", ver);

    /* Allocate buffers */
    DmaBuf src_luma, src_chroma;
    DmaBuf ref_luma[2], ref_chroma[2], ref_sluma[2];
    DmaBuf rec_luma, rec_chroma, rec_sluma;
    DmaBuf mb_info, unk_buf, vle_out;

    if (dmabuf_alloc(&src_luma,   width * height)         < 0) goto cleanup;
    if (dmabuf_alloc(&src_chroma, width * height / 2)     < 0) goto cleanup;
    if (dmabuf_alloc(&ref_luma[0],   luma_sz)  < 0) goto cleanup;
    if (dmabuf_alloc(&ref_luma[1],   luma_sz)  < 0) goto cleanup;
    if (dmabuf_alloc(&ref_chroma[0], chroma_sz)< 0) goto cleanup;
    if (dmabuf_alloc(&ref_chroma[1], chroma_sz)< 0) goto cleanup;
    if (dmabuf_alloc(&ref_sluma[0],  sluma_sz) < 0) goto cleanup;
    if (dmabuf_alloc(&ref_sluma[1],  sluma_sz) < 0) goto cleanup;
    if (dmabuf_alloc(&rec_luma,      luma_sz)  < 0) goto cleanup;
    if (dmabuf_alloc(&rec_chroma,    chroma_sz)< 0) goto cleanup;
    if (dmabuf_alloc(&rec_sluma,     sluma_sz) < 0) goto cleanup;
    if (dmabuf_alloc(&mb_info,   mbinfo_sz)           < 0) goto cleanup;
    if (dmabuf_alloc(&unk_buf,   unk_sz)              < 0) goto cleanup;
    if (dmabuf_alloc(&vle_out,   vle_sz)              < 0) goto cleanup;

    /* Fill source with gray NV12 */
    memset(src_luma.virt,   0x80, width * height);
    memset(src_chroma.virt, 0x80, width * height / 2);

    int ref_idx = 0;
    for (int f = 0; f < frames; f++) {
        int is_idr  = (f == 0);
        int write_h = (f == 0);

        int bytes = encode_frame(
            width, height, qp, is_idr, f & 0xf,
            &src_luma, &src_chroma,
            &ref_luma[ref_idx],   &ref_chroma[ref_idx],  &ref_sluma[ref_idx],
            &rec_luma,            &rec_chroma,            &rec_sluma,
            &mb_info, &unk_buf, &vle_out, write_h);

        if (bytes < 0) {
            fprintf(stderr, "frame %d encode failed\n", f);
            break;
        }

        fprintf(stderr, "frame %d: %d bytes\n", f, bytes);
        fwrite(vle_out.virt, 1, bytes, stdout);

        /* Swap rec→ref for next frame */
        ref_idx ^= 1;
        /* Copy rec into next ref slot */
        memcpy(ref_luma[ref_idx].virt,   rec_luma.virt,   luma_sz);
        memcpy(ref_chroma[ref_idx].virt, rec_chroma.virt, chroma_sz);
        memcpy(ref_sluma[ref_idx].virt,  rec_sluma.virt,  sluma_sz);
        dmabuf_flush(&ref_luma[ref_idx]);
        dmabuf_flush(&ref_chroma[ref_idx]);
        dmabuf_flush(&ref_sluma[ref_idx]);
    }

    fprintf(stderr, "Done.\n");

cleanup:
    ioctl(cedar_fd, IOCTL_RELEASE_LOCK, VE_LOCK_VENC);
    if (cedar_fd >= 0) close(cedar_fd);
    if (heap_fd  >= 0) close(heap_fd);
    return 0;
}
