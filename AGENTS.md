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

Patches: `refs/patch_cedrus/0001-0009` — patches for T527 variant. All patches are unified diffs 
applied with `patch -p1` from the kernel tree root.

**IMPORTANT: Patch format requirement:** The orangepi-build system requires patches in 
`diff -u` format (`--- a/file +++ b/file`), NOT git format (`diff --git` with `index` line). 
Generate patches with:
```sh
diff -u arquivo_original arquivo_modificado > patch.patch
```
Then manually adjust paths to use `a/` and `b/` prefixes.

**IMPORTANT: `refs/` directories are READ-ONLY reference trees.** Do NOT modify files in 
`refs/orange-pi-5.15-sun55iw3/` or `refs/cedrus/`. Instead, copy originals to `/tmp/`, 
make changes there, and generate patches with `diff -u`.

Patch 0005 expects the original unmodified `cedrus_h264.c` (with `vb2_find_timestamp`), 
NOT a pre-patched copy.

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

### CRITICAL: Duplicate device tree node issue (dmesg)
Current dmesg shows TWO device tree nodes for same hardware:
```
cedrus 1c0e000.ve: Device registered as /dev/video1  (WORKS)
cedrus soc@3000000:ve1@1c0e000: IRQ index 0 not found  (FAILS)
cedrus soc@3000000:ve1@1c0e000: Failed to probe hardware
```
The SECOND node (`ve1@1c0e000`) is a duplicate created by patch 0006 or DTS overlay.
It lacks proper IRQ definition, causing probe failure.

**Fix:** Remove the duplicate `ve1@1c0e000` node from DTS. Keep only the working
`1c0e000.ve` node (or vice-versa, ensure only ONE node exists with correct IRQ).
Check: `refs/orange-pi-5.15-sun55iw3/arch/arm64/boot/dts/allwinner/sun55i-t527-orangepi-4a.dts`

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

## Kernel patches

**Root cause of hang (fixes in this session):**

1. **`v4l2_ctrl_request_complete` called before the HW finishes:**
   In `cedrus_device_run()`, `v4l2_ctrl_request_complete()` was called BEFORE
   `dec_ops->trigger()`. The request would transition to COMPLETE before the
   VE hardware even started. Userspace `poll(request_fd, POLLPRI)` returned
   POLLPRI immediately, and the next frame's `device_run()` would overwrite
   VE registers while the previous frame was still in progress → VE lockup.
   **Fix:** replace `v4l2_ctrl_request_complete` in `device_run` with storing
   the request in `ctx->current_req`. Completion moved to the IRQ handler
   (`cedrus_irq`), AFTER the HW finishes.
   (Patch `0005-fix-request-complete-order.patch`, 3 files: `.h`, `.dec.c`, `.hw.c`)

2. **`MEDIA_REQUEST_IOC_REINIT` é no-op no BSP T527:** Após o request completar
   (IRQ handler chama `v4l2_ctrl_request_complete`), o `request_fd` fica em
   estado COMPLETE permanentemente. `media_request_reinit()` retorna 0 mas não
   faz nada. No reuso da mesma surface, `RequestBeginPicture` tenta REINIT →
   no-op → `QBUF(OUTPUT, request_fd=X)` com request COMPLETE → kernel aceita
   mas nunca QUEUED → `media_request_queue` → ENOENT → hardware nunca inicia.
   **Fix no userspace** (`src/picture.c:251-256`): `close()` o request_fd antigo
   e deixa `RequestEndPicture` alocar um novo a cada decode.

**Effect:** Sem estas correções, o ffmpeg decodifica 7 frames e trava no 8º
(pipeline de 28 surfaces em round-robin, trava quando o request_fd reutilizado
atinge o primeiro no-op do REINIT).

### `refs/patch_cedrus/0006-dts-fix-cedrus-compatible.patch` (UNIFIED)
Combines DTS fixes for T527:
1. Fixes the `ve` node in `sun55i-t527-orangepi-4a.dts` to match the cedrus driver:
   - Overrides `clock-names`: `"bus_ve", "ve", "mbus_ve"` → `"ahb", "mod", "ram"`
   - Overrides `reg` with single entry (drops SRAM mbus range)
   - Removes `reset-names`
2. Disables duplicate `ve1@1c0e000` node that causes probe failure:
   - Sets `status = "disabled"` for the duplicate node
   - Prevents "IRQ index 0 not found" error from duplicate node

### `refs/patch_cedrus/0005-fix-request-complete-order.patch`
Fixes the HW lockup at frame 8+ by deferring `v4l2_ctrl_request_complete`
from `cedrus_device_run()` to the IRQ handler (`cedrus_irq()`).

| Hunk | File | Change |
|------|------|--------|
| 1 | `cedrus.h` | Add `struct media_request *current_req` to `struct cedrus_ctx` |
| 2 | `cedrus_dec.c` | Remove `v4l2_ctrl_request_complete()` from `device_run`, store `src_req` in `ctx->current_req` |
| 3 | `cedrus_hw.c` | Call `v4l2_ctrl_request_complete(ctx->current_req)` in `cedrus_irq()` BEFORE `v4l2_m2m_buf_done_and_job_finish()` |

- Patch is a plain unified diff (`diff -u`)+format, applied with `patch -p1`
- Without this patch: `device_run` completes the request before `trigger()`,
  userspace polls POLLPRI and submits next frame while VE is still processing
  → VE register overwrite → HW lockup on frame 8
- `0005-cedrus-fix-vb2_find_timestamp.patch` is **NOT needed** because
  `v4l2_m2m_buf_copy_metadata()` in `device_run` already sets
  `copied_timestamp=1` before `cedrus_write_frame_list()` runs

**To apply (via orangepi-build):**
Copy patches 0001-0006 to `userpatches/kernel/sun55iw3-current/` in the orangepi-build tree.
The build system applies patches with `patch -p1` in alphanumeric order: 0001 → 0006.
**IMPORTANT:** The old `0005-cedrus-fix-vb2_find_timestamp.patch` is **obsolete** and must NOT be used.
The correct patch 0005 is `0005-fix-request-complete-order.patch`.

**To apply manually:**

```sh
cd /home/orangepi/orangepi-build/kernel/orange-pi-5.15-sun55iw3

# 1) Register T527 variant and NO_SRAM + DTS clock fix
patch -p1 < /path/to/refs/patch_cedrus/0001-cedrus-add-sunxi-cedar-ve-compatible.patch
patch -p1 < /path/to/refs/patch_cedrus/0002-cedrus-add-no-sram-capability.patch
patch -p1 < /path/to/refs/patch_cedrus/0004-cedrus-skip-sram-for-t527.patch

# 2) Fix request-complete order (PREVENTS HW LOCKUP)
patch -p1 < /path/to/refs/patch_cedrus/0005-fix-request-complete-order.patch

# 3) Fix DTS clock-names for cedrus compatibility
patch -p1 < /path/to/refs/patch_cedrus/0006-dts-fix-cedrus-compatible.patch

# Rebuild module
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
- `is_queued_in_v4l2`, `capture_requeued` — various CAPTURE requeue attempts
- `v4l2_dequeue_buffer_nonblock()` — caused "Illegal instruction" on BSP T527
- `total_queued_count`, `generate_deterministic_ts()`, `v4l2_drain_capture_queue()`

### Bug 6: Userspace error handling on poll timeout (Session 06/05/2026)
**Problem:** When `poll(POLLPRI)` times out after 7 frames, buffers remain queued in
kernel. Reusing the surface causes `QBUF` to fail with EINVAL (buffer already queued).

**Fixes applied (userspace):**
1. `picture.c:358-363`: Set `capture_queued = true` after QBUF CAPTURE
2. `picture.c:246-249`: Only clear `capture_queued` if DQBUF succeeds in `RequestBeginPicture`
3. `surface.c:276`: Clear `capture_queued` after successful DQBUF CAPTURE
4. `surface.c:288-298`: Error handler in `RequestSyncSurface` tries non-blocking
   DQBUF on OUTPUT and CAPTURE buffers; only clears `capture_queued` if DQBUF CAPTURE succeeds
5. `surface.c:298`: Set `status = VASurfaceReady` (not VASurfaceRendering) in error path
6. `request.c:178`: Open video_fd with `O_NONBLOCK` to prevent DQBUF from blocking
7. `media.c:118-120`: Check `POLLPRI` in revents — treat missing POLLPRI as timeout

**Root cause still in kernel:** The hardware hangs after 7 frames. Log shows
`poll rc=0 revents=0x0` (timeout). IRQ handler may not be called, or
`v4l2_ctrl_request_complete()` may not be signaling the poll.

**This session (07-09/05/2026):**
- Patch 0006: regenerado em `diff -u` puro; desabilita `ve1@1c0e000` no DTS
- Patch 0007: regenerado em `diff -u` limpo (debug IRQ handler)
- Patch 0008: forward declaration de `cedrus_watchdog_callback` adicionada
- Patch 0009: contexto `return 0` removido (função é `void`), alinhado com `}` real

**Bugs corrigidos no userspace (`v4l2_translation.c`):**

| Bug | Local | Sintoma | Correção |
|---|---|---|---|
| Bug 7: PRED_WEIGHTS condicional | `v4l2_translation.c:352` | Stale B-frame weights corrompem P-frames (Bug 2, AGENTS.md) | `translate_pred_weights()` SEMPRE chamado, `need_pred_weights = 1` fixo |
| Bug 8: Flag `DIRECT_SPATIAL_MV_PRED` | `v4l2_translation.c:234` | Internal=0x04, kernel define=0x01 → check nunca batia, B-frame sem direct MV | `src->flags & 0x04` → `dst->flags \|= 0x01` (valores explícitos) |
| Bug 9: Flag `SP_FOR_SWITCH` | `v4l2_translation.c:236` | Internal=0x08, kernel define=0x02 → idem, SP-frame corrompido | `src->flags & 0x08` → `dst->flags \|= 0x02` |

**Problema remanescente (frames 2-5 ainda com lixo):**
HW decodifica sem erro (IRQ status=2, todos os frames). I-frame (frame 1) OK. P/B-frames corrompidos → predição inter-quadros falhando.

**Hipótese principal:** `cedrus_write_frame_list()` usa `vb2_find_timestamp()` para buscar frames de referência na fila CAPTURE. Se o buffer de referência não tem `copied_timestamp=true` ou o timestamp não corresponde, o kernel pula a referência e o HW decodifica sem ela.

Veredito: `dpb_lookup` (h264.c:99-119) busca na DPB local por `picture_id`. `dpb_fill_dpb` (h264.c:194-233) itera `context->dpb.entries[i]` e preenche `decode->dpb[i]`. A ordem da iteração (índice 0→H264_DPB_SIZE) não precisa corresponder à ordem das referências do VAAPI. Mas a VAAPI pode não estar populando `VAPictureParameterBufferH264.ReferenceFrames[]` corretamente via ffmpeg.

**Teste imediato:** Compilar com `ninja -C build && sudo ninja -C build install` e testar:
```sh
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i "test/Cavaleiros do Zodíaco 1h.264.mp4" -vframes 10 -f null -
```

**Next steps (kernel debugging):**
- Add `printk` to `cedrus_irq()` to verify IRQ is firing
- Add `printk` to `cedrus_device_run()` to verify trigger is happening
- Verify `v4l2_ctrl_request_complete()` is called in IRQ handler (patch 0005)
- Check if HW is hanging due to incorrect V4L2 controls (SPS/PPS/SLICE/DECODE)
