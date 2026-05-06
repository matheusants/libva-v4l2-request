# libva-v4l2-request

libVA backend using the Linux V4L2 Request API for hardware video decode on Allwinner SoCs.

## Build

```sh
meson setup build && ninja -C build
sudo ninja -C build install
```

Output is `v4l2_request_drv_video.so` installed to `${libdir}/dri`.

Dependencies: `libva >= 1.1.0`, `libdrm >= 2.4.52`, Linux kernel headers with V4L2 Request API support.

## Custom kernel headers

```sh
meson setup build -Dkernel_headers=/path/to/linux
```

## Key structure

| Path | Purpose |
|---|---|
| `src/request.c` | VAAPI driver entry point |
| `src/v4l2.c`, `src/media.c`, `src/video.c` | V4L2/media abstraction layer |
| `src/surface.c` | Capture buffer management |
| `src/context.c` | Codec session (sets v4l output format + profile fix) |
| `src/picture.c` | Encoded frame submission (request API) |
| `src/buffer.c` | VA buffer management |
| `src/image.c` | NV12/tiled-to-linear conversion |
| `src/mpeg2.c`, `src/h264.c`, `src/h265.c` | Per-codec V4L2 control setup |
| `src/v4l2_translation.c` | Internal `_internal` structs → modern kernel UAPI |
| `src/tiled_yuv.S`, `src/tiled_yuv_aarch64.S` | sunxi tiled format conversion |

Test video: `test/Cavaleiros do Zodíaco 1h.264.mp4` — H264 High Profile, 1440x1080, 23.98 fps, yuv420p.

Patches: `refs/patch_cedrus/0001-0006` — patches for T527 variant. All patches are unified diffs applied with `patch -p1` from the kernel tree root. Patch 0005 expects the original unmodified `cedrus_h264.c` (with `vb2_find_timestamp`), NOT a pre-patched copy.

Kernel source: `refs/orange-pi-5.15-sun55iw3/` (full orangepi-build kernel tree, BSP T527).

## Pipeline flow

- `RequestCreateSurfaces2`: detects NV12, registers surface objects
- `RequestCreateContext`: `S_FMT OUTPUT` → profile fix → `S_FMT CAPTURE` → `CREATE_BUFS` → `QUERYBUF` → `mmap` → `STREAMON`
- `RequestBeginPicture`: if rendering → `RequestSyncSurface`; close old `request_fd` → alocar novo em `RequestEndPicture` (REINIT é no-op no BSP T527)
- `RequestEndPicture`: `codec_set_controls` → `QBUF CAPTURE` (no request_fd) → `gettimeofday` → `QBUF OUTPUT` (with request_fd + timestamp) → `RequestSyncSurface`
- `RequestSyncSurface`: `media_request_queue` → `poll POLLPRI` → `DQBUF OUTPUT` → `DQBUF CAPTURE`

## V4L2 layer quirks

- `v4l2_set_control` uses `V4L2_CTRL_WHICH_REQUEST_VAL` (via request_fd). CUR_VAL causes pipeline hang.
- `v4l2_dequeue_buffer` does NOT pre-set `buffer.index` — kernel picks next available.
- `v4l2_set_control_simple()` for integer/menu controls (uses `control.value`, not `control.ptr`).
- All `v4l2_set_control` compound path uses `control.ptr` for codec controls.

## BSP T527 kernel quirks (OrangePi 4A)

- `select()` does NOT work — use `poll(POLLPRI)` (already patched in `media.c`).
- `MEDIA_REQUEST_IOC_REINIT` is a no-op.
- `/dev/media0` = ISP, `/dev/media1` = Cedrus VE.
- **Deadly** (require reboot): QBUF of already-QUEUED buffer; `poll(POLLIN)` without buffers; DQBUF CAPTURE separated from DQBUF OUTPUT; DQBUF of unprocessed buffer.

### Profile control

BSP defaults `h264_profile` to Main (2). High Profile (4) corrupts P/B frames. Fixed in `context.c:143-153`:
```c
v4l2_set_control_simple(video_fd, 0x00990a6b, 4); /* V4L2_H264_PROFILE_HIGH */
```

### DPB management (h264.c)

Structures in `h264.h`:
- `H264_DPB_SIZE = 16` — circular DPB slots
- `H264_TS_CACHE_SIZE = 20` — timestamp cache for reinserted references
- Each entry: `{pic, age, used, valid, reserved, timestamp}`
- `ts_cache` maps `VASurfaceID → timestamp`, survives `dpb_clear_entry()`

Rules:
- `dpb_find_invalid_entry`: do NOT skip `reserved=1` entries
- `dpb_find_oldest_unused_entry`: MUST skip `reserved=1` entries (Bug 4 fix)
- `dpb_clear_entry(entry, true)`: preserve `entry->pic` before `memset`
- `dpb_lookup`: include `reserved` entries (not only `valid`)
- `dpb_insert`: skip only if `existing->valid && !existing->reserved`

`h264_set_controls` order:
1. `dpb_lookup`/`dpb_find_entry` → pick slot for CurrPic
2. `dpb_clear_entry(slot, true)` → reserve slot
3. `dpb_update()` → mark references, reinsert missing via `ts_cache`
4. `h264_va_picture_to_v4l2()` → `h264_fill_dpb()` fills DPB (without CurrPic)
5. `h264_va_matrix_to_v4l2()`, `h264_va_slice_to_v4l2()`
6. `h264_translate_and_set_controls()` → sends DPB + controls to kernel
7. `dpb_insert(CurrPic)` → adds CurrPic to DPB for future references

**Why CurrPic NOT in DPB sent to kernel (item 4):**
The kernel's `cedrus_write_frame_list()` iterates all VALID DPB entries.
For each, it reads the buffer's `codec.h264.position` (set during that
buffer's previous decode) and marks `used_dpbs |= BIT(position)`.
If CurrPic is in the DPB, the kernel finds its buffer, reads position=0
(all buffers start at position=0 from kzalloc), and sets `output = 0`.
This causes CurrPic AND reference frames to share position=0 in the SRAM
framebuffer list, overwriting reference luma/chroma pointers.

Without CurrPic in the DPB, `output = -1` → the kernel calls
`find_first_zero_bit(&used_dpbs, 18)` which assigns a unique position.

### Translation layer (v4l2_translation.c)

| Struct | Internal → Modern | CID |
|--------|-------------------|-----|
| SPS | 1048 → 1048 | `0xa40902` |
| PPS | 12 → 12 | `0xa40903` |
| MATRIX | 480 → 480 | `0xa40904` |
| PRED_WEIGHTS | 772 → 772 | `0xa40905` (sempre enviado) |
| SLICE | 892 → 152 | `0xa40906` |
| DECODE | 496 → 560 | `0xa40907` |

- `ref_pic_list0/1`: internal `__u8[32]` → modern `struct v4l2_h264_reference[32]`
  with `{fields, index}`. Fields mapped from VAAPI flags via
  `ref_pic_list{0,1}_fields[32]` in the internal struct.
- `v4l2_h264_dpb_entry`: 24→32 bytes (reordered, `pic_num` u16→u32, new `fields` field)
- `pred_weight_table`: removed from SLICE, sent as separate CID `0xa40905`
  unconditionally (zeroed when not required) to overwrite stale residual
  from no-op `MEDIA_REQUEST_IOC_REINIT`.

## Kernel patches (required for P/B frame fix)

**Root cause:**

1. **`copied_timestamp` gate in `vb2_find_timestamp`:** O BSP T527 em
   `drivers/media/common/videobuf2/videobuf2-v4l2.c:653`
   exige `q->bufs[i]->copied_timestamp == 1`. A flag `copied_timestamp` é setada
   APENAS por `v4l2_m2m_buf_copy_metadata()` no callback `device_run` do cedrus
   (`cedrus_dec.c:86`), que roda **antes** de `cedrus_write_frame_list`.
   **NÃO tentar setar `copied_timestamp=1` durante QBUF(CAPTURE)** — isso trava
   o pipeline BSP T527.

2. **Timestamp collision on CAPTURE buffer reuse:** When a CAPTURE buffer is
   reused for a new decode (same `dst_idx`), its `vb2_buf.timestamp` still has
   the OLD frame's timestamp. If the old frame is still in the DPB as a
   reference, `cedrus_write_frame_list()` matches `run->dst->vb2_buf.timestamp
   == dpb->reference_ts`, triggering `output = position; continue;`. This uses
   the REFERENCE's old SRAM position for the CURRENT output AND skips
   `cedrus_fill_ref_pic` for the reference → `luma_ptr=0` → Y=0.

3. **`MEDIA_REQUEST_IOC_REINIT` é no-op no BSP T527.** Após o primeiro uso
   de um `request_fd`, o request fica em estado COMPLETE e não aceita novos
   controles. Tentar REINIT não muda o estado → `QBUF(OUTPUT)` com esse
   `request_fd` falha → hardware nunca inicia → POLL timeout → loop infinito.
   **Fix no userspace** (`src/picture.c:225-230`): fechar o `request_fd` antigo
   e alocar um novo para cada decode.

**Effect:** Reference CAPTURE buffers with stale timestamps match `run->dst`
→ SRAM position corrupted → P/B frames Y=0/garbage.

### `refs/patch_cedrus/0006-dts-fix-cedrus-compatible.patch`
Fixes the `ve` node in `sun55i-t527-orangepi-4a.dts` to match the cedrus driver:
- Overrides `clock-names`: `"bus_ve", "ve", "mbus_ve"` → `"ahb", "mod", "ram"`
  (cedrus calls `devm_clk_get(dev, "ahb")`, the Allwinner VE names are different)
- Overrides `reg` with single entry (drops SRAM mbus range at `0x03000000`):
  T527 uses `CEDRUS_CAPABILITY_NO_SRAM`, doesn't need the SRAM register mapping
- Removes `reset-names` (the cedrus driver calls `devm_reset_control_get(dev, NULL)`
  — it doesn't look up by name)

### `refs/patch_cedrus/0005-cedrus-fix-vb2_find_timestamp.patch`
Supplementary fix in `cedrus_h264.c`. 6 hunks:

| Hunk | Function | Change |
|------|----------|--------|
| 1 | `cedrus_write_frame_list` | Add `unsigned int j;` variable |
| 2 | `cedrus_write_frame_list` | Replace `vb2_find_timestamp()` with manual `for` loop over `cap_q->bufs[j]->timestamp`, add `!= &run->dst->vb2_buf` skip to avoid timestamp collision on CAPTURE reuse |
| 3 | `cedrus_write_frame_list` | Remove `if (run->dst->vb2_buf.timestamp == dpb->reference_ts)` block (falsely triggered by stale timestamps) |
| 4 | `cedrus_write_frame_list` | Replace `if (output >= 0) position = output;` with unconditional `find_first_zero_bit` |
| 5 | `_cedrus_write_ref_list` | Add `unsigned int j;` variable |
| 6 | `_cedrus_write_ref_list` | Replace `vb2_find_timestamp()` with manual `for` loop (no `run->dst` skip needed — this function only iterates valid DPB references) |

- Patch is a plain unified diff (`diff -u` format), applied with `patch -p1` from the kernel tree root.
- Expects the **original** BSP kernel's `cedrus_h264.c` (with `vb2_find_timestamp`), NOT a pre-modified copy.
- **CRITICAL:** If the kernel tree was previously modified (e.g., from a failed patch attempt), run `git checkout -- drivers/staging/media/sunxi/cedrus/cedrus_h264.c` first, or the patch context will not match.

**To apply (via orangepi-build):**
Copy all patches to `userpatches/kernel/sun55iw3-current/` in the orangepi-build tree.
The build system applies patches with `patch -p1` in alphanumeric order: 0001 → 0006.

**To apply manually:**
```sh
cd /home/orangepi/orangepi-build/kernel/orange-pi-5.15-sun55iw3
patch -p1 < /path/to/refs/patch_cedrus/0006-dts-fix-cedrus-compatible.patch
patch -p1 < /path/to/refs/patch_cedrus/0005-cedrus-fix-vb2_find_timestamp.patch
make M=drivers/staging/media/sunxi/cedrus modules
sudo make M=drivers/staging/media/sunxi/cedrus modules_install
sudo depmod -a
sudo modprobe -r sunxi-cedrus && sudo modprobe sunxi-cedrus
```

**Rebuild user-space driver after kernel update:**
```sh
cd /path/to/libva-v4l2-request
ninja -C build && sudo ninja -C build install
```

**Test:**
```sh
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i test/video.mp4 \
  -vframes 5 /tmp/frame%02d.png
```

## Userspace bugs found and fixed

### Bug 1: DPB slot reuse conflict — STALE POINTER (h264.c:586-640)

`h264_set_controls()` obtains an `output` pointer via `dpb_lookup`/`dpb_find_entry`,
then calls `dpb_clear_entry(output, true)` which sets `reserved=true`. After
`dpb_update()`, the pointer can become STALE: `dpb_update()` internally calls
`dpb_find_oldest_unused_entry()` which can return the reserved slot (age=0 from
memset) and reinsert a reference frame there. The final `dpb_insert(CurrPic,
output)` then overwrites the reference with CurrPic → reference frame silently
lost → P/B frame missing reference → garbage.

**Fix (stale pointer only):** Save `output_idx = output - context->dpb.entries` before `dpb_update()`,
recover `output = &context->dpb.entries[output_idx]` after.

### Bug 4: DPB slot theft by `dpb_find_oldest_unused_entry` (h264.c:128-145)

`dpb_find_oldest_unused_entry()` did NOT check for `entry->reserved`. After
`dpb_clear_entry(output, true)`, the reserved slot has `used=false, age=0`
(both zeroed by memset). When `dpb_update()` calls `dpb_find_entry()` →
`dpb_find_oldest_unused_entry()`, the slot with age=0 (the reserved slot)
always wins. `dpb_update` inserts a REFERENCE frame into the reserved slot,
destroying the reservation. Then `dpb_insert(CurrPic, output, ts)` at line 654
overwrites that reference with CurrPic → reference frame silently lost →
P/B frame missing reference → Y=0/garbage.

**Root cause:** After Bug 1's `output_idx` fix, `output` correctly points to
`output_idx`, but `dpb_update` has already placed a REFERENCE there, not
CurrPic. The `dpb_insert(CurrPic, output, ts)` call overwrites the reference.

**Fix:** Add `!entry->reserved` to the condition in `dpb_find_oldest_unused_entry`:
```c
if (!entry->used && !entry->reserved && (entry->age < min_age))
```
This prevents `dpb_update` from ever selecting the reserved slot for reference
reinsertion. The reserved slot is now truly reserved for CurrPic only.

### Bug 2: Stale PRED_WEIGHTS from no-op REINIT (v4l2_translation.c)

`MEDIA_REQUEST_IOC_REINIT` is a no-op on BSP T527. If a B-frame sends PRED_WEIGHTS
(`weighted_bipred_idc=1`), the control REMAINS in the request for the next frame.
If the next P-frame does NOT send PRED_WEIGHTS (was conditional on
`V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED`), the kernel applies the stale B-frame's
prediction weights → wrong motion compensation → P-frame corruption.

**Fix:** Always call `translate_pred_weights()` and always send
`V4L2_CID_STATELESS_H264_PRED_WEIGHTS`, even when not required. A zeroed
pred_weights struct (= no weighting) overwrites the stale residual.

### Bug 3: Timestamp collision from gettimeofday (picture.c:318)

`gettimeofday()` has microsecond resolution. Fast consecutive decodes (or
concurrent threads) can produce identical timestamps, causing
`vb2_find_timestamp` to return the wrong buffer (first match by index). With
the kernel patch that bypasses `copied_timestamp`, this is even more likely
since ALL buffers are searched linearly.

**Fix:** Add a static monotonic counter to `tv_usec` that ensures uniqueness.

### Bug 5: `MEDIA_REQUEST_IOC_REINIT` no-op → infinite loop (picture.c:225)

The BSP T527's `MEDIA_REQUEST_IOC_REINIT` is a no-op — after the first decode
completes, the `request_fd` stays in state COMPLETE and cannot accept new
controls or buffers. `RequestBeginPicture` called `media_request_reinit()` which
returned success but did nothing. The subsequent `QBUF(OUTPUT)` with the stale
request_fd succeeded (kernel associates it) but the request never transitions
to QUEUED → hardware never starts → `poll(POLLPRI)` times out → userspace
retries in an infinite loop.

**Fix:** Replace `media_request_reinit()` with `close()` + new alloc:

```c
// picture.c:225-230
if (surface_object->request_fd >= 0) {
    close(surface_object->request_fd);
    surface_object->request_fd = -1;
}
```

`RequestEndPicture` already allocates a new `request_fd` when
`surface_object->request_fd < 0`.

### Fields removed (do NOT add back)
- `capture_queued`, `is_queued_in_v4l2`, `capture_requeued` — various CAPTURE requeue attempts
- `v4l2_dequeue_buffer_nonblock()` — caused "Illegal instruction" on BSP T527
- `total_queued_count`, `generate_deterministic_ts()`, `v4l2_drain_capture_queue()`
