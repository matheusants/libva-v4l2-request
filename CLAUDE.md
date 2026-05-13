# CLAUDE.md

Guidance for Claude Code when working in this repo.

## Build

```sh
# Configure (once)
meson setup build

# Build
ninja -C build

# Install
sudo ninja -C build install
```

Output: `build/src/v4l2_request_drv_video.so` → installed to `${libdir}/dri`.

Custom kernel headers:
```sh
meson setup build -Dkernel_headers=/path/to/linux
```

Rebuild cedrus kernel module after kernel changes:
```sh
cd /home/orangepi/orangepi-build/kernel/orange-pi-5.15-sun55iw3
make M=drivers/staging/media/sunxi/cedrus modules
sudo make M=drivers/staging/media/sunxi/cedrus modules_install
sudo depmod -a && sudo modprobe -r sunxi-cedrus && sudo modprobe sunxi-cedrus
```

## Test

```sh
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i "test/Cavaleiros do Zodíaco 1h.264.mp4" -vframes 10 -f null -
```

Watch kernel logs: `dmesg | grep cedrus` or `dmesg | grep cedrus_dbg`.

## Architecture

VAAPI backend driver (shared lib loaded by libva). Bridges VAAPI → Linux V4L2 Request API. Targets Allwinner T527 SoC (OrangePi 4A) + Cedrus VE hardware decoder.

### Data flow

```
ffmpeg/app → libva API → v4l2_request_drv_video.so → V4L2 Request API → cedrus kernel driver → VE hardware
```

### Key source files

| File | Role |
|---|---|
| `src/request.c` | VAAPI driver entry point, function table (`VADriverVTable`) |
| `src/video.c` | V4L2 video device (`/dev/videoN`) open/format/stream ops |
| `src/media.c` | Media controller (`/dev/media1`) — request alloc, queue, poll |
| `src/v4l2.c` | Low-level V4L2 ioctls (QBUF, DQBUF, S_FMT, controls) |
| `src/context.c` | Codec session lifecycle — `S_FMT` OUTPUT+CAPTURE, `STREAMON`, profile fix |
| `src/picture.c` | Per-frame decode: `BeginPicture` / `EndPicture` / request lifecycle |
| `src/surface.c` | CAPTURE buffer pool, `SyncSurface` (poll + DQBUF) |
| `src/buffer.c` | VA buffer management (wraps codec parameter buffers) |
| `src/h264.c` | H264 DPB management + `h264_set_controls()` |
| `src/v4l2_translation.c` | Translates Bootlin internal structs → modern kernel UAPI structs |
| `src/image.c` | NV12/tiled-to-linear YUV conversion for `GetImage` |
| `include/h264-ctrls.h` | Old Bootlin internal H264 control structs (source side of translation) |
| `refs/v4l2-controls.h` | Modern kernel UAPI H264 structs (target side of translation) |

### Per-frame decode sequence

1. `RequestBeginPicture`: if surface rendering → `SyncSurface`; `close()` old `request_fd`
2. Codec fills VA buffers (SPS, PPS, slice data via `RenderPicture`)
3. `RequestEndPicture`: `codec_set_controls` → `QBUF CAPTURE` → `QBUF OUTPUT` (with new `request_fd` + timestamp) → `SyncSurface`
4. `RequestSyncSurface`: `media_request_queue` → `poll(POLLPRI)` → `DQBUF OUTPUT` → `DQBUF CAPTURE`

### Translation layer (`v4l2_translation.c`)

Bootlin original code used pre-upstream H264 control structs (`include/h264-ctrls.h`). T527 BSP kernel uses modern UAPI (`refs/v4l2-controls.h`). `v4l2_translation.c` converts field-by-field. CIDs: SPS=`0xa40902`, PPS=`0xa40903`, MATRIX=`0xa40904`, PRED_WEIGHTS=`0xa40905`, SLICE=`0xa40906`, DECODE=`0xa40907`.

## Critical BSP T527 quirks

- `/dev/media0` = ISP, `/dev/media1` = Cedrus VE
- `select()` broken — use `poll(POLLPRI)` only
- `MEDIA_REQUEST_IOC_REINIT` no-op — `close()` and realloc `request_fd` each frame
- `v4l2_set_control` uses `V4L2_CTRL_WHICH_CUR_VAL` (which=0) — BSP T527 does NOT use REQUEST_VAL
- `v4l2_dequeue_buffer` must NOT pre-set `buffer.index` — kernel picks next available
- **Deadly** (require reboot): QBUF already-queued buffer; `poll(POLLIN)` without buffers; DQBUF CAPTURE without DQBUF OUTPUT; DQBUF unprocessed buffer
- H264 profile: BSP defaults to Main (2). High Profile requires `v4l2_set_control_simple(video_fd, 0x00990a6b, 4)` in `context.c`

## H264 DPB rules (h264.c)

DPB has 16 slots (`H264_DPB_SIZE`) + 20-entry timestamp cache (`H264_TS_CACHE_SIZE`).

Critical invariants:
- `dpb_find_invalid_entry`: do NOT skip `reserved=1` entries
- `dpb_find_oldest_unused_entry`: MUST skip `reserved=1` entries (prevents slot theft)
- `dpb_clear_entry(entry, true)`: preserves `entry->pic` before memset
- `dpb_lookup`: include `reserved` entries (not only `valid`)
- CurrPic inserted into DPB AFTER `h264_translate_and_set_controls()` — kernel must NOT see CurrPic in DPB it receives (causes position=0 collision in SRAM)

Save `output_idx` before `dpb_update()`, recover pointer after — `dpb_update` may move reserved slot.

## Kernel patches

Live patches: `/home/orangepi/orangepi-build/userpatches/kernel/sun55iw3-current/`
Applied alphabetically by orangepi-build. Generated via `git diff HEAD` on kernel tree,
stripped of `diff --git` / `index` lines so `patch -p1` applies cleanly.

Consolidated patches (8 files, replacing 15 individual patches):

| Patch | File | Purpose |
|---|---|---|
| 0001-t527-cedrus-core | cedrus.c | T527 variant (600MHz, caps) + sunxi-cedar-ve compatible |
| 0002-t527-cedrus-ctx-irq | cedrus.h + cedrus_dec.c | NO_SRAM capability + defer request_complete to IRQ |
| 0003-t527-cedrus-hw | cedrus_hw.c | Skip SRAM claim + NV12/TILED fmt fix + TILED stride + IRQ completion + VE_MBUS1 IOMMU |
| 0004-t527-cedrus-h264 | cedrus_h264.c | Skip pic_list pos 0 + re-apply dst_format + MIXED_RAM bufs |
| 0005-t527-cedrus-mpeg2 | cedrus_mpeg2.c | Re-apply dst_format after engine_enable |
| 0006-t527-dts | sun55i-t527-orangepi-4a.dts | reg/clock-names/iommus master 2/disable ve1/add ve_enc node |
| 0007-t527-cedrus-capture-dma-sg | cedrus.h + cedrus_video.c + Kconfig | CAPTURE queue vb2_dma_sg → cacheable CPU mmap → 151fps GetImage |
| 0010-t527-cedar-enc-compat | bsp/drivers/ve/cedar-ve/cedar_ve.c | sunxi-ve: add "sunxi-cedar-ve-enc" compat + poll mode when no IRQ |

To regenerate a patch after editing kernel source in-tree:
```sh
cd /home/orangepi/orangepi-build/kernel/orange-pi-5.15-sun55iw3
git diff HEAD -- drivers/staging/media/sunxi/cedrus/<file>.c | grep -v "^diff --git\|^index " > /path/to/patch
```

**`refs/` directories read-only.** Never modify files there directly.

## Current status (2026-05-12)

**H264 decode: FULLY WORKING.** All frame types (I/P/B) correct. Verified via libva (ffmpeg)
and GStreamer (v4l2slh264dec). ~192fps at 1440×1080 with `-hwaccel_output_format vaapi`
(frames stay in HW). ~151fps with GetImage (vb2_dma_sg + 8-thread parallel copy).

**H265 decode: FULLY WORKING.** Verified via libva (ffmpeg). ~192fps at 1440×1080
with `-hwaccel_output_format vaapi`. GStreamer: `vah265dec` crashes (driver incompatibility);
no `v4l2slh265dec` in GST 1.20.3.

**MPEG2 decode: FULLY WORKING.** Verified via libva (ffmpeg) and GStreamer (v4l2slmpeg2dec).
~21fps at 1440×1080. Uses 3-control stateless API (SEQUENCE + PICTURE + QUANTISATION).

GetImage optimization (H264/H265, 22fps → 151fps):
1. `src/tiled_yuv_aarch64.S`: tile-by-tile ASM iteration (cache-friendly tile reads)
2. `src/tiled_yuv_mt.c`: 8-thread pool splitting tile rows across T527 A55 cores → 3×
3. Kernel patch 0007: CAPTURE queue `vb2_dma_sg` → cacheable CPU mmap + auto cache sync → 7× total
4. `src/context.c` + `src/image.c`: dma-buf EXPBUF + remap + `DMA_BUF_IOCTL_SYNC`

Root cause of H264/H265 P/B garbage: T527 VE dual MBUS masters — VE_MBUS1 (master 3,
MC reference reads) not bound to IOMMU. Fixed by kernel patches 0003 (VE_MBUS1 IOMMU
in cedrus_hw.c) + 0006 (DTS iommus master 2).

Chroma offset bug (affected all codecs at 1080p): `context.c` computed chroma plane
offset using VAAPI `picture_height` (1080) instead of kernel-aligned `fmt_height` (1088
for TILED). Fixed by reading `fmt_height` from `G_FMT` after `CREATE_BUFS`.