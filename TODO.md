# libva-v4l2-request — TODO & Status

## Status (2026-05-12)

**H264 HW decode: WORKING** — All frame types (I/P/B) correct on T527 cedrus.
Verified via libva (ffmpeg -hwaccel vaapi) and GStreamer (v4l2slh264dec).
~192fps at 1440×1080 with -hwaccel_output_format vaapi (frames stay in HW).
~22fps without it — bottleneck is vaGetImage tiled→linear CPU copy in image.c.

**H265 HW decode: WORKING** — 4-control stateless API (SPS + PPS + DECODE_PARAMS + SLICE_PARAMS).
Verified via libva (ffmpeg). ~192fps at 1440×1080 (hwaccel_output_format vaapi).
GStreamer: vah265dec (GST 1.20.3 native VA plugin) crashes with v4l2_request — no fix planned.

**MPEG2 HW decode: WORKING** — 3-control stateless API (SEQUENCE + PICTURE + QUANTISATION).
Verified via libva (ffmpeg) and GStreamer (v4l2slmpeg2dec). ~21fps at 1440×1080.

Root causes resolved:
- H264/H265 P/B garbage: T527 VE_MBUS1 (MC ref reads) not bound to IOMMU -> kernel patches
- Chroma offset mismatch: context.c used VAAPI height (1080) vs kernel-aligned height (1088 TILED) -> fixed

---

## Active kernel patches

6 consolidated patches in /home/orangepi/orangepi-build/userpatches/kernel/sun55iw3-current/:

| Patch | File | Purpose |
|---|---|---|
| 0001-t527-cedrus-core | cedrus.c | T527 variant + compatible |
| 0002-t527-cedrus-ctx-irq | cedrus.h + cedrus_dec.c | NO_SRAM cap + IRQ request_complete |
| 0003-t527-cedrus-hw | cedrus_hw.c | SRAM skip + NV12/TILED fmt + MBUS1 IOMMU |
| 0004-t527-cedrus-h264 | cedrus_h264.c | pos0 skip + dst_format reapply + MIXED_RAM |
| 0005-t527-cedrus-mpeg2 | cedrus_mpeg2.c | dst_format reapply after engine_enable |
| 0006-t527-dts | sun55i-t527-orangepi-4a.dts | reg/clocks/iommus master 2/disable ve1 |

Disabled (debug): 0008-watchdog-timeout, 0009-dump-h264-params
Backed up (superseded individual patches): backup_individual/

---

## TODO

- [ ] **Optimize tiled_to_planar (src/image.c + tiled_yuv_aarch64.S)** — vaGetImage
      tiled→linear CPU copy costs ~8.7× throughput (192fps → 22fps at 1440×1080).
      Profile cache behavior; 32×32 tile traversal likely cache-unfriendly for large stride.
      NEON prefetch or reorder copy loop may recover significant fps.

- [ ] **Frame quality cross-reference** — compare decoded frames vs another HW platform
      to verify pixel-accurate output (noted frames look correct but sizes seem large)

- [ ] **Research HW encoding on OrangePi 4A** — T527 VE may support H264/H265 encode;
      check cedrus encode support upstream; check BSP kernel for encode ioctls;
      check libva-v4l2-request Bootlin code for encode stubs; assess feasibility

- [ ] **Tag release** — v1.0-t527 once frame cross-reference confirmed

---

## Test commands

```sh
# ffmpeg -- HW decode benchmark (frames stay in HW, no GetImage overhead)
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi \
  -i "test/Cavaleiros do Zodíaco 1h.264.mp4" -t 30 -f null -

# ffmpeg -- HW decode with GetImage (measures full pipeline including tiled->linear)
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i "test/Cavaleiros do Zodíaco 1h.264.mp4" -t 30 -f null -

# GStreamer -- H264 direct V4L2 stateless
gst-launch-1.0 filesrc location="test/Cavaleiros do Zodíaco 1h.264.mp4" ! \
  qtdemux ! h264parse ! v4l2slh264dec ! fakesink sync=false

# GStreamer -- MPEG2
gst-launch-1.0 filesrc location="test/mpeg2_60s.m2v" ! \
  mpegvideoparse ! v4l2slmpeg2dec ! fakesink sync=false

# Frame extraction for visual inspection
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -ss 60 -i "test/Cavaleiros do Zodíaco 1h.264.mp4" \
  -vframes 10 test/output/h264_frame%02d.png
```
