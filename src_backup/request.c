/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "buffer.h"
#include "config.h"
#include "context.h"
#include "image.h"
#include "picture.h"
#include "subpicture.h"
#include "surface.h"

#include "autoconfig.h"

#include <va/va_backend.h>

#include "request.h"
#include "utils.h"
#include "v4l2.h"
#include "h264-ctrls.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/media.h>

/* Set default visibility for the init function only. */
VAStatus __attribute__((visibility("default")))
VA_DRIVER_INIT_FUNC(VADriverContextP context);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP context)
{
	struct request_data *driver_data;
	struct VADriverVTable *vtable = context->vtable;
	VAStatus status;
	unsigned int capabilities;
	unsigned int capabilities_required;
	int video_fd = -1;
	int media_fd = -1;
	char *video_path;
	char *media_path;
	int rc;

	context->version_major = VA_MAJOR_VERSION;
	context->version_minor = VA_MINOR_VERSION;
	context->max_profiles = V4L2_REQUEST_MAX_PROFILES;
	context->max_entrypoints = V4L2_REQUEST_MAX_ENTRYPOINTS;
	context->max_attributes = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;
	context->max_image_formats = V4L2_REQUEST_MAX_IMAGE_FORMATS;
	context->max_subpic_formats = V4L2_REQUEST_MAX_SUBPIC_FORMATS;
	context->max_display_attributes = V4L2_REQUEST_MAX_DISPLAY_ATTRIBUTES;
	context->str_vendor = V4L2_REQUEST_STR_VENDOR;

	vtable->vaTerminate = RequestTerminate;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigProfiles = RequestQueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = RequestQueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = RequestQueryConfigAttributes;
	vtable->vaCreateConfig = RequestCreateConfig;
	vtable->vaDestroyConfig = RequestDestroyConfig;
	vtable->vaGetConfigAttributes = RequestGetConfigAttributes;
	vtable->vaCreateSurfaces = RequestCreateSurfaces;
	vtable->vaCreateSurfaces2 = RequestCreateSurfaces2;
	vtable->vaDestroySurfaces = RequestDestroySurfaces;
	vtable->vaExportSurfaceHandle = RequestExportSurfaceHandle;
	vtable->vaCreateContext = RequestCreateContext;
	vtable->vaDestroyContext = RequestDestroyContext;
	vtable->vaCreateBuffer = RequestCreateBuffer;
	vtable->vaBufferSetNumElements = RequestBufferSetNumElements;
	vtable->vaMapBuffer = RequestMapBuffer;
	vtable->vaUnmapBuffer = RequestUnmapBuffer;
	vtable->vaDestroyBuffer = RequestDestroyBuffer;
	vtable->vaBufferInfo = RequestBufferInfo;
	vtable->vaAcquireBufferHandle = RequestAcquireBufferHandle;
	vtable->vaReleaseBufferHandle = RequestReleaseBufferHandle;
	vtable->vaBeginPicture = RequestBeginPicture;
	vtable->vaRenderPicture = RequestRenderPicture;
	vtable->vaEndPicture = RequestEndPicture;
	vtable->vaSyncSurface = RequestSyncSurface;
	vtable->vaQuerySurfaceAttributes = RequestQuerySurfaceAttributes;
	vtable->vaQuerySurfaceStatus = RequestQuerySurfaceStatus;
	vtable->vaPutSurface = RequestPutSurface;
	vtable->vaQueryImageFormats = RequestQueryImageFormats;
	vtable->vaCreateImage = RequestCreateImage;
	vtable->vaDeriveImage = RequestDeriveImage;
	vtable->vaDestroyImage = RequestDestroyImage;
	vtable->vaSetImagePalette = RequestSetImagePalette;
	vtable->vaGetImage = RequestGetImage;
	vtable->vaPutImage = RequestPutImage;
	vtable->vaQuerySubpictureFormats = RequestQuerySubpictureFormats;
	vtable->vaCreateSubpicture = RequestCreateSubpicture;
	vtable->vaDestroySubpicture = RequestDestroySubpicture;
	vtable->vaSetSubpictureImage = RequestSetSubpictureImage;
	vtable->vaSetSubpictureChromakey = RequestSetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = RequestSetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = RequestAssociateSubpicture;
	vtable->vaDeassociateSubpicture = RequestDeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = RequestQueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = RequestGetDisplayAttributes;
	vtable->vaSetDisplayAttributes = RequestSetDisplayAttributes;
	vtable->vaLockSurface = RequestLockSurface;
	vtable->vaUnlockSurface = RequestUnlockSurface;

	driver_data = malloc(sizeof(*driver_data));
	memset(driver_data, 0, sizeof(*driver_data));

	context->pDriverData = driver_data;

	object_heap_init(&driver_data->config_heap,
			 sizeof(struct object_config), CONFIG_ID_OFFSET);
	object_heap_init(&driver_data->context_heap,
			 sizeof(struct object_context), CONTEXT_ID_OFFSET);
	object_heap_init(&driver_data->surface_heap,
			 sizeof(struct object_surface), SURFACE_ID_OFFSET);
	object_heap_init(&driver_data->buffer_heap,
			 sizeof(struct object_buffer), BUFFER_ID_OFFSET);
	object_heap_init(&driver_data->image_heap, sizeof(struct object_image),
			 IMAGE_ID_OFFSET);

	/*video_path = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	if (video_path == NULL)
		video_path = "/dev/video0"; */

	video_path = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	if (video_path == NULL) {
        	/* Enumera /dev/video0..9 e usa o primeiro que for V4L2_CAP_VIDEO_M2M */
	        static char found_path[16];
        	int probe_fd;
	        unsigned int probe_caps;
        	int n;
	        video_path = "/dev/video1"; /* fallback seguro para Cedrus */
        	for (n = 0; n <= 9; n++) {
                	snprintf(found_path, sizeof(found_path), "/dev/video%d", n);
	                //probe_fd = open(found_path, O_RDWR | O_NONBLOCK);
	                probe_fd = open(found_path, O_RDWR);
        	        if (probe_fd < 0) continue;
                	if (v4l2_query_capabilities(probe_fd, &probe_caps) == 0 &&
	                    (probe_caps & V4L2_CAP_VIDEO_M2M)) {
        	                close(probe_fd);
                	        video_path = found_path;
                        	break;
	                }
        	        close(probe_fd);
	        }
	}

	//video_fd = open(video_path, O_RDWR | O_NONBLOCK);
	video_fd = open(video_path, O_RDWR);

	if (video_fd < 0) {
                perror("DEBUG: open video failed");
                return VA_STATUS_ERROR_OPERATION_FAILED;
        }

	rc = v4l2_query_capabilities(video_fd, &capabilities);
	if (rc < 0) {
		//fprintf(stderr, "DEBUG: v4l2_query_capabilities FAILED\n");
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	//fprintf(stderr, "DEBUG: capabilities=0x%x\n", capabilities);

	capabilities_required = V4L2_CAP_STREAMING;

	if ((capabilities & capabilities_required) != capabilities_required) {
		request_log("Missing required driver capabilities\n");
		//fprintf(stderr, "DEBUG: missing V4L2_CAP_STREAMING\n");
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	media_path = getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH");
	media_fd = -1;

	if (media_path) {
		media_fd = open(media_path, O_RDWR | O_NONBLOCK);
        	//if (media_fd >= 0) fprintf(stderr, "DEBUG: Usando media_path da ENV: %s\n", media_path);
	}

    	if (media_fd < 0) {
        	char path[32];
        	struct media_device_info info;
        	for (int i = 0; i < 16; i++) {
            	snprintf(path, sizeof(path), "/dev/media%d", i);
            	int fd = open(path, O_RDWR | O_NONBLOCK);
            	if (fd < 0) continue;

            	if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
                	if (strcmp(info.driver, "cedrus") == 0) {
                    	media_fd = fd;
                    	//fprintf(stderr, "DEBUG: Cedrus auto-detectado em %s\n", path);
                    	break;
                	}
            	}
            	close(fd);
        	}
    	}

	/*if (media_fd < 0) {
	        fprintf(stderr, "ERRO: Media Device do Cedrus não encontrado! Hardware transcoding falhará.\n");
    	} */

	driver_data->video_fd = video_fd;
	driver_data->media_fd = media_fd;

	//fprintf(stderr, "DEBUG: init SUCCESS\n");

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	status = VA_STATUS_ERROR_OPERATION_FAILED;

	//fprintf(stderr, "DEBUG: init FAILED\n");

	if (video_fd >= 0)
		close(video_fd);

	if (media_fd >= 0)
		close(media_fd);

complete:
	return status;
}

VAStatus RequestTerminate(VADriverContextP context)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_buffer *buffer_object;
	struct object_image *image_object;
	struct object_surface *surface_object;
	struct object_context *context_object;
	struct object_config *config_object;
	int iterator;

	close(driver_data->video_fd);
	close(driver_data->media_fd);

	/* Cleanup leftover buffers. */

	image_object = (struct object_image *)
		object_heap_first(&driver_data->image_heap, &iterator);
	while (image_object != NULL) {
		RequestDestroyImage(context, (VAImageID)image_object->base.id);
		image_object = (struct object_image *)
			object_heap_next(&driver_data->image_heap, &iterator);
	}

	object_heap_destroy(&driver_data->image_heap);

	buffer_object = (struct object_buffer *)
		object_heap_first(&driver_data->buffer_heap, &iterator);
	while (buffer_object != NULL) {
		RequestDestroyBuffer(context,
				     (VABufferID)buffer_object->base.id);
		buffer_object = (struct object_buffer *)
			object_heap_next(&driver_data->buffer_heap, &iterator);
	}

	object_heap_destroy(&driver_data->buffer_heap);

	surface_object = (struct object_surface *)
		object_heap_first(&driver_data->surface_heap, &iterator);
	while (surface_object != NULL) {
		RequestDestroySurfaces(context,
				      (VASurfaceID *)&surface_object->base.id, 1);
		surface_object = (struct object_surface *)
			object_heap_next(&driver_data->surface_heap, &iterator);
	}

	object_heap_destroy(&driver_data->surface_heap);

	context_object = (struct object_context *)
		object_heap_first(&driver_data->context_heap, &iterator);
	while (context_object != NULL) {
		RequestDestroyContext(context,
				      (VAContextID)context_object->base.id);
		context_object = (struct object_context *)
			object_heap_next(&driver_data->context_heap, &iterator);
	}

	object_heap_destroy(&driver_data->context_heap);

	config_object = (struct object_config *)
		object_heap_first(&driver_data->config_heap, &iterator);
	while (config_object != NULL) {
		RequestDestroyConfig(context,
				     (VAConfigID)config_object->base.id);
		config_object = (struct object_config *)
			object_heap_next(&driver_data->config_heap, &iterator);
	}

	object_heap_destroy(&driver_data->config_heap);

	free(context->pDriverData);
	context->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}
