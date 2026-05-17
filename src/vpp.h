/*
 * Video post-processing (VAEntrypointVideoProc) for the v4l2-request driver.
 *
 * Provides the VPP entrypoint Jellyfin's scale_vaapi filter needs between the
 * cedrus decoder and the sunxi-venc encoder. The T527 has no general-purpose
 * scaler reachable here, so the scale/convert is done in software (NV12).
 */

#ifndef _VPP_H_
#define _VPP_H_

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_vpp.h>

struct request_data;
struct object_context;

VAStatus RequestQueryVideoProcFilters(VADriverContextP context,
				      VAContextID context_id,
				      VAProcFilterType *filters,
				      unsigned int *filters_count);
VAStatus RequestQueryVideoProcFilterCaps(VADriverContextP context,
					 VAContextID context_id,
					 VAProcFilterType type,
					 void *filter_caps,
					 unsigned int *filter_caps_count);
VAStatus RequestQueryVideoProcPipelineCaps(VADriverContextP context,
					   VAContextID context_id,
					   VABufferID *filters,
					   unsigned int filters_count,
					   VAProcPipelineCaps *pipeline_caps);

/* Run the latched VPP frame: scale the input surface into render_surface_id. */
VAStatus vpp_process_picture(struct request_data *driver_data,
			     struct object_context *context_object);

#endif
