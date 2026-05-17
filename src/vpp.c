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

		for (dx = 0; dx < j->dw; dx++) {
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

	sw = in->width;
	sh = in->height;
	sstride = in->destination_bytesperlines[0];
	if (sstride < sw)
		sstride = sw;
	dw = out->width;
	dh = out->height;
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/*
	 * The decoder CAPTURE buffer is linear NV12 (kernel patch 0007) — scale
	 * straight from it. A tiled buffer is detiled into a scratch copy first.
	 */
	if (video_format_is_linear(driver_data->video_format)) {
		src_y = in->destination_data[0];
		if (in->destination_planes_count >= 2)
			src_uv = in->destination_data[1];
		else
			src_uv = (const uint8_t *)in->destination_data[0] +
				 (size_t)sstride * sh;
	} else {
		detiled = malloc((size_t)sstride * sh * 3 / 2);
		if (detiled == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		tiled_to_planar_mt(in->destination_data[0], detiled, sstride,
				   sstride, sh);
		tiled_to_planar_mt(in->destination_planes_count >= 2 ?
				   in->destination_data[1] :
				   (uint8_t *)in->destination_data[0] +
				   (size_t)sstride * sh,
				   detiled + (size_t)sstride * sh, sstride,
				   sstride, sh / 2);
		src_y = detiled;
		src_uv = detiled + (size_t)sstride * sh;
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
