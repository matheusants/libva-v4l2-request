# v4l2-request libVA Backend — Allwinner T527 (OrangePi 4A)

> **T527 fork** of [bootlin/libva-v4l2-request](https://github.com/bootlin/libva-v4l2-request).
> Adds H264/H265/MPEG2 hardware decode and H264 VAAPI encode for the Allwinner T527 SoC
> (OrangePi 4A, BSP kernel 5.15-sun55iw3).

## Allwinner T527 — Quick Start

### 1. Apply the DTS patch (required)

The cedrus VE node in the OrangePi 4A device tree must be patched before building the kernel.
Using [orangepi-build](https://github.com/orangepi-xunlong/orangepi-build):

```sh
# Copy the patch into the orangepi-build userpatches directory
cp dts-patches/0007-t527-dts.patch \
   ~/orangepi-build/userpatches/kernel/sun55iw3-current/

# Rebuild and install the kernel
cd ~/orangepi-build
sudo ./build.sh BOARD=orangepi4a BRANCH=current BUILD_OPT=kernel KERNEL_CONFIGURE=no
sudo dpkg -i output/debs/linux-image-*.deb output/debs/linux-dtb-*.deb
sudo reboot
```

The patch adjusts the cedrus node: correct register range, clock names, IOMMU master 2
binding, and disables the unused ve1 node.

### 2. Build and install the kernel driver

Use the [cedrus-t527](https://github.com/matheusants/cedrus-t527) driver (decode only)
or [sunxi-venc-t527](https://github.com/matheusants/sunxi-venc-t527) (decode + encode).
Place the driver files at `drivers/staging/media/sunxi/cedrus/` in the kernel tree and
include them in the orangepi-build userpatches, then rebuild.

### 3. Build and install this library

```sh
meson setup build
ninja -C build
sudo ninja -C build install
```

Set `LIBVA_DRIVER_NAME=v4l2_request` and `LIBVA_DRIVERS_PATH` to the install path.

### Test

```sh
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i input.mp4 -vframes 10 -f null -
```

### Performance (792 MHz VE)

| Workload | fps |
|---|---|
| 4K H264 → 4K H264 (VAAPI transcode) | 26 fps / 1.08× realtime |
| 4K H264 → 720p H264 | 37 fps / 1.54× |
| 1440×1080 HEVC → H264 | 110 fps / 4.58× |
| 4K H264 decode-only | ~40 fps |

---

## About

This libVA backend is designed to work with the Linux Video4Linux2
Request API that is used by a number of video codecs drivers,
including the Video Engine found in most Allwinner SoCs.

## Status

The v4l2-request libVA backend currently supports the following formats:
* MPEG2 (Simple and Main profiles)
* H264 (Baseline, Main and High profiles)
* H265 (Main profile)

## Instructions

In order to use this libVA backend, the `v4l2_request` driver has to
be specified through the `LIBVA_DRIVER_NAME` environment variable, as
such:

	export LIBVA_DRIVER_NAME=v4l2_request

A media player that supports VAAPI (such as VLC) can then be used to decode a
video in a supported format:

	vlc path/to/video.mpg

Sample media files can be obtained from:

	http://samplemedia.linaro.org/MPEG2/
	http://samplemedia.linaro.org/MPEG4/SVT/

## Technical Notes

### Surface

A Surface is an internal data structure never handled by the VA's user
containing the output of a rendering. Usualy, a bunch of surfaces are created
at the begining of decoding and they are then used alternatively. When
created, a surface is assigned a corresponding v4l capture buffer and it is
kept until the end of decoding. Syncing a surface waits for the v4l buffer to
be available and then dequeue it.

Note: since a Surface is kept private from the VA's user, it can ask to
directly render a Surface on screen in an X Drawable. Some kind of
implementation is available in PutSurface but this is only for development
purpose.

### Context

A Context is a global data structure used for rendering a video of a certain
format. When a context is created, input buffers are created and v4l's output
(which is the compressed data input queue, since capture is the real output)
format is set.

### Picture

A Picture is an encoded input frame made of several buffers. A single input
can contain slice data, headers and IQ matrix. Each Picture is assigned a
request ID when created and each corresponding buffer might be turned into a
v4l buffers or extended control when rendered. Finally they are submitted to
kernel space when reaching EndPicture.

The real rendering is done in EndPicture instead of RenderPicture
because the v4l2 driver expects to have the full corresponding
extended control when a buffer is queued and we don't know in which
order the different RenderPicture will be called.

### Image

An Image is a standard data structure containing rendered frames in a usable
pixel format. Here we only use NV12 buffers which are converted from sunxi's
proprietary tiled pixel format with tiled_yuv when deriving an Image from a
Surface.
