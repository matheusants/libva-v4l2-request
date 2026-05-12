/*
 * SUNXI Tiled NV12 -> linear converter, AArch64 NEON
 * Tile size: 32x32 bytes (1024 bytes/tile).
 *
 * Iteration order: tile-by-tile (ty, tx outer; r inner).
 * Each tile's 1024 bytes are read sequentially, fitting in A55 L1D (32KB).
 * Row-major order caused L1 thrashing: 45 tiles * 1024B = 45KB > 32KB L1.
 */

#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

#ifdef __aarch64__

.text
.align 2

.macro aarch64_function fname
    .global \fname
    .type \fname, %function
\fname:
.endm

/*
 * tiled_to_planar
 *   x0: src  (tiled base)
 *   x1: dst  (linear base)
 *   x2: pitch (dst bytes per row)
 *   x3: width  (pixels)
 *   x4: height (rows)
 *
 * Callee-saved: x19-x26
 * Scratch:      x5-x17, v0-v7
 */
aarch64_function tiled_to_planar
    stp x19, x20, [sp, #-64]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]

    mov x19, x0         // src
    mov x20, x1         // dst
    mov x21, x2         // pitch
    mov x22, x3         // width
    mov x23, x4         // height

    // tiles_per_row = (width + 31) >> 5
    add x24, x22, #31
    lsr x24, x24, #5

    // tile_rows = (height + 31) >> 5
    add x25, x23, #31
    lsr x25, x25, #5

    mov x26, #0         // ty = 0

.Ltp_ty_loop:
    cmp x26, x25
    b.ge .Ltp_done

    // rows_this_tile = min(height - ty*32, 32)
    lsl x5, x26, #5            // ty * 32
    sub x5, x23, x5            // height - ty*32
    cmp x5, #32
    b.le .Ltp_rows_ok
    mov x5, #32
.Ltp_rows_ok:                  // x5 = rows_this_tile

    // dst_tile_base = dst + ty * 32 * pitch
    lsl x6, x26, #5            // ty * 32
    mul x6, x6, x21
    add x6, x20, x6            // x6 = dst + ty*32*pitch

    mov x7, #0          // tx = 0

.Ltp_tx_loop:
    cmp x7, x24
    b.ge .Ltp_tx_done

    // src_tile = src + (ty * tiles_per_row + tx) * 1024
    mul x8, x26, x24
    add x8, x8, x7
    lsl x8, x8, #10            // * 1024
    add x8, x19, x8            // x8 = src_tile

    // prefetch start of next tile (tx+1 is contiguous in memory)
    prfm PLDL1KEEP, [x8, #1024]

    // dst_col = tx * 32
    lsl x9, x7, #5             // x9 = tx * 32

    mov x10, #0         // r = 0

.Ltp_r_loop:
    cmp x10, x5
    b.ge .Ltp_r_done

    // src_ptr = src_tile + r * 32
    lsl x11, x10, #5
    add x11, x8, x11

    // dst_ptr = dst_tile_base + r * pitch + dst_col
    mul x12, x10, x21
    add x12, x6, x12
    add x12, x12, x9

    ld1 {v0.16b, v1.16b}, [x11]
    st1 {v0.16b, v1.16b}, [x12]

    add x10, x10, #1
    b .Ltp_r_loop

.Ltp_r_done:
    add x7, x7, #1
    b .Ltp_tx_loop

.Ltp_tx_done:
    add x26, x26, #1
    b .Ltp_ty_loop

.Ltp_done:
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #64
    ret
.size tiled_to_planar, .-tiled_to_planar


/*
 * tiled_deinterleave_to_planar  (unused by image.c; kept for completeness)
 *   x0: src    (tiled NV12 UV base)
 *   x1: dst_u
 *   x2: dst_v
 *   x3: pitch
 *   x4: width
 *   x5: height
 *
 * Same tile-by-tile strategy. Uses ld2 to deinterleave UV.
 */
aarch64_function tiled_deinterleave_to_planar
    stp x19, x20, [sp, #-80]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]

    mov x19, x0         // src
    mov x20, x1         // dst_u
    mov x21, x2         // dst_v
    mov x22, x3         // pitch
    mov x23, x4         // width
    mov x24, x5         // height

    // tiles_per_row = (width + 31) >> 5
    add x25, x23, #31
    lsr x25, x25, #5

    // tile_rows = (height + 31) >> 5
    add x26, x24, #31
    lsr x26, x26, #5

    mov x27, #0         // ty

.Ldip_ty_loop:
    cmp x27, x26
    b.ge .Ldip_done

    // rows_this_tile = min(height - ty*32, 32)
    lsl x5, x27, #5
    sub x5, x24, x5
    cmp x5, #32
    b.le .Ldip_rows_ok
    mov x5, #32
.Ldip_rows_ok:

    // dst_u_tile_base = dst_u + ty*32*pitch
    lsl x6, x27, #5
    mul x6, x6, x22
    add x6, x20, x6

    // dst_v_tile_base = dst_v + ty*32*pitch
    add x28, x21, x6
    sub x28, x28, x20   // offset = ty*32*pitch
    add x28, x21, x28

    // redo cleanly
    lsl x6, x27, #5
    mul x6, x6, x22
    add x6, x20, x6     // x6 = dst_u + ty*32*pitch
    add x7, x21, x6
    sub x7, x7, x20     // wrong, redo
    // dst_v_base = dst_v + ty*32*pitch
    lsl x7, x27, #5
    mul x7, x7, x22
    add x7, x21, x7     // x7 = dst_v + ty*32*pitch

    mov x8, #0          // tx

.Ldip_tx_loop:
    cmp x8, x25
    b.ge .Ldip_tx_done

    // src_tile = src + (ty * tpr + tx) * 1024
    mul x9, x27, x25
    add x9, x9, x8
    lsl x9, x9, #10
    add x9, x19, x9    // x9 = src_tile

    prfm PLDL1KEEP, [x9, #1024]

    // dst_col_half = tx * 16  (each 32-byte UV tile row → 16 U + 16 V)
    lsl x10, x8, #4

    mov x11, #0         // r

.Ldip_r_loop:
    cmp x11, x5
    b.ge .Ldip_r_done

    // src_ptr = src_tile + r * 32
    lsl x12, x11, #5
    add x12, x9, x12

    // dst_u_ptr = dst_u_base + r*pitch + dst_col_half
    mul x13, x11, x22
    add x13, x6, x13
    add x13, x13, x10

    // dst_v_ptr = dst_v_base + r*pitch + dst_col_half
    mul x14, x11, x22
    add x14, x7, x14
    add x14, x14, x10

    ld2 {v0.16b, v1.16b}, [x12]
    st1 {v0.16b}, [x13]
    st1 {v1.16b}, [x14]

    add x11, x11, #1
    b .Ldip_r_loop

.Ldip_r_done:
    add x8, x8, #1
    b .Ldip_tx_loop

.Ldip_tx_done:
    add x27, x27, #1
    b .Ldip_ty_loop

.Ldip_done:
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #80
    ret
.size tiled_deinterleave_to_planar, .-tiled_deinterleave_to_planar

#endif
