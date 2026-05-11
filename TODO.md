# libva-v4l2-request — TODO & Status

## Status (2026-05-11)

**H264 HW decode: WORKING** — All frame types (I/P/B) decode correctly on T527 cedrus.
Verified via libva (ffmpeg -hwaccel vaapi) and GStreamer (v4l2slh264dec) independently.

Root cause of P/B garbage was dual MBUS IOMMU: VE_MBUS1 (master 3, MC reference reads)
not bound to IOMMU. Fix: kernel patches 0006 (DTS master 2) + 0017 (sunxi_enable_device_iommu(3)).

---

## Active kernel patches

| Patch | Purpose |
|---|---|
| 0001 | Add `sunxi-cedar-ve` compatible → cedrus probes T527 |
| 0002 | Add `CEDRUS_CAPABILITY_NO_SRAM` flag |
| 0003 | Define `sun55i_t527_cedrus_variant` (600MHz, caps) |
| 0004 | Skip `sunxi_sram_claim()` for T527 |
| 0005 | Move `v4l2_ctrl_request_complete` to IRQ handler (prevents frame 8+ lockup) |
| 0006 | DTS: reg/clock-names/iommus master 2/disable duplicate ve1 |
| 0011 | NV12 secondary out fmt + chroma len (dead code on TILED path, valid for NV12 output) |
| 0013 | Skip pic_list position 0 (sram_byte=0 = null sentinel on T527) |
| 0014 | Set chroma_buf_len + stride for TILED format |
| 0015 | Re-apply dst_format after engine_enable (T527 resets regs) |
| 0016 | Force MIXED_RAM bufs for NO_SRAM capability |
| 0017 | `sunxi_enable_device_iommu(3, true)` — enable IOMMU for VE_MBUS1 (THE fix) |

Removed debug patches: 0007, 0010, 0012.

---

## TODO

- [ ] **Commit userspace changes** — stage and commit all modified `src/` files (h264.c,
      v4l2_translation.c, context.c, surface.c, v4l2.c, picture.c, request.c, media.c)
      with meaningful commit message documenting the T527 quirks fixed

- [ ] **Clean debug printks from main branch** — scan all `src/` files for leftover
      `fprintf`/debug log calls added during P/B investigation; remove before tagging

- [ ] **Branch: H265 + MPEG2 evaluation** — create `codec/h265-mpeg2` branch;
      wire up H265 and MPEG2 codec paths (already stubbed in cedrus variant 0003);
      test with sample files; likely needs new translation structs in v4l2_translation.c

- [ ] **Research HW encoding on OrangePi 4A** — T527 VE may support H264/H265 encode;
      check cedrus encode support upstream; check if BSP kernel has encode ioctls;
      check if libva-v4l2-request Bootlin code has encode stubs; assess feasibility

---

## Test commands

```sh
# libva path — 30s transcode
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i "test/Cavaleiros do Zodíaco 1h.264.mp4" \
  -t 30 -c:v libx264 -preset ultrafast -crf 23 -c:a copy \
  test/output/test_30s.mp4

# GStreamer path — direct V4L2
gst-launch-1.0 filesrc location="test/Cavaleiros do Zodíaco 1h.264.mp4" ! \
  qtdemux ! h264parse ! v4l2slh264dec ! fakesink sync=false
```
