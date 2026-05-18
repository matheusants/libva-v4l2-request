/*
 * Video post-processing (VAEntrypointVideoProc) — software NV12 scale/convert.
 *
 * Jellyfin's scale_vaapi filter sits between the cedrus decoder and the
 * sunxi-venc encoder. The T527 exposes no general scaler here, so the work is
 * done on the CPU: the decoded NV12 surface is bilinearly resampled into the
 * output surface's heap buffer, which the encoder then consumes.
 */

#include "vpp.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "context.h"
#include "request.h"
#include "surface.h"
#include "video.h"
#include "tiled_yuv.h"
#include "utils.h"

VAStatus RequestQueryVideoProcFilters(VADriverContextP context,
				      VAContextID context_id,
				      VAProcFilterType *filters,
				      unsigned int *filters_count)
{
	/* Scaling and NV12<->NV12 passthrough are implicit — no named filters. */
	(void)context;
	(void)context_id;
	(void)filters;
	*filters_count = 0;
	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryVideoProcFilterCaps(VADriverContextP context,
					 VAContextID context_id,
					 VAProcFilterType type,
					 void *filter_caps,
					 unsigned int *filter_caps_count)
{
	(void)context;
	(void)context_id;
	(void)type;
	(void)filter_caps;
	*filter_caps_count = 0;
	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryVideoProcPipelineCaps(VADriverContextP context,
					   VAContextID context_id,
					   VABufferID *filters,
					   unsigned int filters_count,
					   VAProcPipelineCaps *pipeline_caps)
{
	(void)context;
	(void)context_id;
	(void)filters;
	(void)filters_count;

	if (pipeline_caps == NULL)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	/* No references, no rotation/blend/mirror — a plain scaler. */
	memset(pipeline_caps, 0, sizeof(*pipeline_caps));
	return VA_STATUS_SUCCESS;
}

/* Clamp an integer to [0, hi]. */
static inline int clampi(int v, int hi)
{
	return v < 0 ? 0 : (v > hi ? hi : v);
}

/* Bilinear weights use an 8-bit fraction (0..256). */
#define VPP_FRAC_BITS	8
#define VPP_FRAC_ONE	(1 << VPP_FRAC_BITS)
#define VPP_MAX_THREADS	8

/*
 * Map a destination index to a source sample: x0 = base pixel, frac = 0..256
 * weight of the next pixel. Centre-aligned (the +0.5 / -0.5 terms).
 */
static void build_axis(int dst_n, int src_n, int *x0, int *frac)
{
	int i;

	for (i = 0; i < dst_n; i++) {
		/* s = (i + 0.5) * src/dst - 0.5, in 8-bit fixed point. */
		long s = ((long)(2 * i + 1) * src_n * VPP_FRAC_ONE) /
			 (2 * dst_n) - (VPP_FRAC_ONE / 2);
		int base = (int)(s >> VPP_FRAC_BITS);

		x0[i] = base;
		frac[i] = (int)(s - ((long)base << VPP_FRAC_BITS));
	}
}

struct scale_job {
	const uint8_t *src;
	uint8_t *dst;
	int sh, sstride, dw, dstride, nch;
	const int *xb, *xf;	/* per-column base pixel / fraction */
	const int *yb, *yf;	/* per-row base pixel / fraction */
	int y_start, y_end;	/* destination rows for this worker */
};

#if defined(__aarch64__)
#include <arm_neon.h>

/*
 * NEON bilinear resample of one luma (nch=1) destination row, 16 pixels at a
 * time. The horizontal gather uses vqtbl4q over a 64-byte source window, so a
 * group is only handled when its 16 taps span <=62 bytes (downscale ratio
 * <~4x) and the window stays inside the source row. Returns the number of
 * leading pixels written; the caller resamples the remainder in scalar.
 */
static int scale_row_y_neon(const uint8_t *r0, const uint8_t *r1, int fy,
			    const int *xb, const int *xf, uint8_t *drow,
			    int dw, int sstride)
{
	int16x8_t fyv = vdupq_n_s16((int16_t)fy);
	int dx;

	for (dx = 0; dx + 16 <= dw; dx += 16) {
		int wbase = xb[dx];
		int k;
		int32x4_t w = vdupq_n_s32(wbase);
		int32x4_t b0, b1, b2, b3, f0, f1, f2, f3;
		uint8x16x4_t t0, t1;
		uint8x16_t idx0, idx1, p00, p01, p10, p11;
		uint8x8_t outh[2];
		int16x8_t fxl, fxh;

		if (xb[dx + 15] - wbase > 62 || wbase + 64 > sstride)
			break;

		/* idx = xb - wbase (-> u8), fx = xf (-> s16), built in NEON. */
		b0 = vsubq_s32(vld1q_s32(xb + dx), w);
		b1 = vsubq_s32(vld1q_s32(xb + dx + 4), w);
		b2 = vsubq_s32(vld1q_s32(xb + dx + 8), w);
		b3 = vsubq_s32(vld1q_s32(xb + dx + 12), w);
		idx0 = vcombine_u8(
			vqmovun_s16(vcombine_s16(vmovn_s32(b0), vmovn_s32(b1))),
			vqmovun_s16(vcombine_s16(vmovn_s32(b2), vmovn_s32(b3))));
		idx1 = vaddq_u8(idx0, vdupq_n_u8(1));
		f0 = vld1q_s32(xf + dx);
		f1 = vld1q_s32(xf + dx + 4);
		f2 = vld1q_s32(xf + dx + 8);
		f3 = vld1q_s32(xf + dx + 12);
		fxl = vcombine_s16(vmovn_s32(f0), vmovn_s32(f1));
		fxh = vcombine_s16(vmovn_s32(f2), vmovn_s32(f3));

		t0 = vld1q_u8_x4(r0 + wbase);
		t1 = vld1q_u8_x4(r1 + wbase);
		p00 = vqtbl4q_u8(t0, idx0);
		p01 = vqtbl4q_u8(t0, idx1);
		p10 = vqtbl4q_u8(t1, idx0);
		p11 = vqtbl4q_u8(t1, idx1);

		/* Process the low and high 8-lane halves identically. */
		for (k = 0; k < 2; k++) {
			int16x8_t a0 = vreinterpretq_s16_u16(vmovl_u8(
				k ? vget_high_u8(p00) : vget_low_u8(p00)));
			int16x8_t a1 = vreinterpretq_s16_u16(vmovl_u8(
				k ? vget_high_u8(p01) : vget_low_u8(p01)));
			int16x8_t b0 = vreinterpretq_s16_u16(vmovl_u8(
				k ? vget_high_u8(p10) : vget_low_u8(p10)));
			int16x8_t b1 = vreinterpretq_s16_u16(vmovl_u8(
				k ? vget_high_u8(p11) : vget_low_u8(p11)));
			int16x8_t fx = k ? fxh : fxl;
			int16x8_t da = vsubq_s16(a1, a0);
			int16x8_t db = vsubq_s16(b1, b0);
			int32x4_t pal = vshrq_n_s32(vmull_s16(vget_low_s16(da),
						    vget_low_s16(fx)), 8);
			int32x4_t pah = vshrq_n_s32(vmull_s16(vget_high_s16(da),
						    vget_high_s16(fx)), 8);
			int32x4_t pbl = vshrq_n_s32(vmull_s16(vget_low_s16(db),
						    vget_low_s16(fx)), 8);
			int32x4_t pbh = vshrq_n_s32(vmull_s16(vget_high_s16(db),
						    vget_high_s16(fx)), 8);
			int16x8_t top = vaddq_s16(a0,
				vcombine_s16(vmovn_s32(pal), vmovn_s32(pah)));
			int16x8_t bot = vaddq_s16(b0,
				vcombine_s16(vmovn_s32(pbl), vmovn_s32(pbh)));
			int16x8_t vd = vsubq_s16(bot, top);
			int32x4_t vl = vshrq_n_s32(vmull_s16(vget_low_s16(vd),
						   vget_low_s16(fyv)), 8);
			int32x4_t vh = vshrq_n_s32(vmull_s16(vget_high_s16(vd),
						   vget_high_s16(fyv)), 8);
			int16x8_t out = vaddq_s16(top,
				vcombine_s16(vmovn_s32(vl), vmovn_s32(vh)));
			outh[k] = vqmovun_s16(out);
		}
		vst1q_u8(drow + dx, vcombine_u8(outh[0], outh[1]));
	}
	return dx;
}
#endif

/* Integer bilinear resample of one destination row band. */
static void *scale_worker(void *arg)
{
	const struct scale_job *j = arg;
	int dx, dy, c;

	for (dy = j->y_start; dy < j->y_end; dy++) {
		int y0 = clampi(j->yb[dy], j->sh - 1);
		int y1 = clampi(j->yb[dy] + 1, j->sh - 1);
		int fy = j->yf[dy];
		const uint8_t *r0 = j->src + (size_t)y0 * j->sstride;
		const uint8_t *r1 = j->src + (size_t)y1 * j->sstride;
		uint8_t *drow = j->dst + (size_t)dy * j->dstride;

		dx = 0;
#if defined(__aarch64__)
		if (j->nch == 1)
			dx = scale_row_y_neon(r0, r1, fy, j->xb, j->xf,
					      drow, j->dw, j->sstride);
#endif
		for (; dx < j->dw; dx++) {
			int x0 = j->xb[dx] * j->nch;
			int x1 = (j->xb[dx] + 1) * j->nch;
			int fx = j->xf[dx];

			for (c = 0; c < j->nch; c++) {
				int top = (r0[x0 + c] * (VPP_FRAC_ONE - fx) +
					   r0[x1 + c] * fx) >> VPP_FRAC_BITS;
				int bot = (r1[x0 + c] * (VPP_FRAC_ONE - fx) +
					   r1[x1 + c] * fx) >> VPP_FRAC_BITS;

				drow[dx * j->nch + c] =
					(uint8_t)((top * (VPP_FRAC_ONE - fy) +
						   bot * fy) >> VPP_FRAC_BITS);
			}
		}
	}
	return NULL;
}

/*
 * Bilinear resample of an 8-bit, nch-interleaved-channel plane, row bands
 * spread across the A55 cores. The x-axis base pixels are pre-clamped so the
 * worker's inner loop needs no bounds check.
 */
static void scale_plane(const uint8_t *src, int sw, int sh, int sstride,
			uint8_t *dst, int dw, int dh, int dstride, int nch)
{
	int *xb, *xf, *yb, *yf;
	pthread_t tid[VPP_MAX_THREADS];
	struct scale_job jobs[VPP_MAX_THREADS];
	int nthreads, i, rows, started = 0;
	long ncpu;

	if (sw == dw && sh == dh) {
		for (i = 0; i < dh; i++)
			memcpy(dst + (size_t)i * dstride,
			       src + (size_t)i * sstride, (size_t)dw * nch);
		return;
	}

	xb = malloc(sizeof(int) * dw * 2);
	yb = malloc(sizeof(int) * dh * 2);
	if (xb == NULL || yb == NULL) {
		free(xb);
		free(yb);
		return;
	}
	xf = xb + dw;
	yf = yb + dh;
	build_axis(dw, sw, xb, xf);
	build_axis(dh, sh, yb, yf);
	/* Pre-clamp x base pixels — the worker indexes xb[dx] and xb[dx]+1. */
	for (i = 0; i < dw; i++)
		xb[i] = clampi(xb[i], sw - 2 < 0 ? 0 : sw - 2);

	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	nthreads = ncpu < 1 ? 1 : (ncpu > VPP_MAX_THREADS ? VPP_MAX_THREADS :
				   (int)ncpu);
	if (nthreads > dh)
		nthreads = dh;
	rows = (dh + nthreads - 1) / nthreads;

	for (i = 0; i < nthreads; i++) {
		jobs[i] = (struct scale_job){
			.src = src, .dst = dst, .sh = sh, .sstride = sstride,
			.dw = dw, .dstride = dstride, .nch = nch,
			.xb = xb, .xf = xf, .yb = yb, .yf = yf,
			.y_start = i * rows,
			.y_end = (i + 1) * rows < dh ? (i + 1) * rows : dh,
		};
		if (jobs[i].y_start >= jobs[i].y_end)
			break;
		if (pthread_create(&tid[started], NULL, scale_worker,
				   &jobs[i]) != 0) {
			scale_worker(&jobs[i]);	/* run inline on failure */
			continue;
		}
		started++;
	}
	for (i = 0; i < started; i++)
		pthread_join(tid[i], NULL);

	free(xb);
	free(yb);
}

VAStatus vpp_process_picture(struct request_data *driver_data,
			     struct object_context *context_object)
{
	struct object_surface *in, *out;
	const uint8_t *src_y, *src_uv;
	uint8_t *dst, *detiled = NULL;
	int sw, sh, sstride, dw, dh;
	unsigned int dst_size;

	in = SURFACE(driver_data, context_object->vpp_input_surface_id);
	out = SURFACE(driver_data, context_object->render_surface_id);
	if (in == NULL || out == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/*
	 * Use the input crop rect when present — the decoder surface is
	 * 16-aligned, but only the content region holds valid pixels; scaling
	 * the padding rows would smear them into the bottom of the output.
	 */
	if (context_object->vpp_src_w > 0) {
		sw = context_object->vpp_src_w;
		sh = context_object->vpp_src_h;
	} else {
		sw = in->width;
		sh = in->height;
	}
	sstride = in->destination_bytesperlines[0];
	if (sstride < in->width)
		sstride = in->width;
	dw = out->width;
	dh = out->height;
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/*
	 * NV12 plane offsets use the surface's allocated (16-aligned) height —
	 * that is where the chroma plane physically starts — while the crop
	 * rect (cx,cy) places the content origin within it.
	 */
	{
	int buf_h = in->height;
	int cx = context_object->vpp_src_x;
	int cy = context_object->vpp_src_y;

	if (video_format_is_linear(driver_data->video_format)) {
		const uint8_t *y_base = in->destination_data[0];
		const uint8_t *uv_base =
			in->destination_planes_count >= 2 ?
			in->destination_data[1] :
			y_base + (size_t)sstride * buf_h;

		src_y = y_base + (size_t)cy * sstride + cx;
		src_uv = uv_base + (size_t)(cy / 2) * sstride + cx;
	} else {
		detiled = malloc((size_t)sstride * buf_h * 3 / 2);
		if (detiled == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		tiled_to_planar_mt(in->destination_data[0], detiled, sstride,
				   sstride, buf_h);
		tiled_to_planar_mt(in->destination_planes_count >= 2 ?
				   in->destination_data[1] :
				   (uint8_t *)in->destination_data[0] +
				   (size_t)sstride * buf_h,
				   detiled + (size_t)sstride * buf_h, sstride,
				   sstride, buf_h / 2);
		src_y = detiled + (size_t)cy * sstride + cx;
		src_uv = detiled + (size_t)sstride * buf_h +
			 (size_t)(cy / 2) * sstride + cx;
	}
	}

	/* Output: tight NV12 in the surface heap buffer the encoder reads. */
	dst_size = (unsigned int)dw * dh * 3 / 2;
	if (out->source_data == NULL || out->source_size < dst_size) {
		free(out->source_data);
		out->source_data = malloc(dst_size);
		if (out->source_data == NULL) {
			free(detiled);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
		out->source_size = dst_size;
	}
	dst = out->source_data;

	/* Luma: full resolution. Chroma: interleaved CbCr at half resolution. */
	scale_plane(src_y, sw, sh, sstride, dst, dw, dh, dw, 1);
	scale_plane(src_uv, sw / 2, sh / 2, sstride,
		    dst + (size_t)dw * dh, dw / 2, dh / 2, dw, 2);

	free(detiled);

	out->status = VASurfaceReady;
	return VA_STATUS_SUCCESS;
}
