/*
 * Port AArch64 do conversor Sunxi Tiled para NV12
 * Otimizado para Orange Pi 4A (Allwinner T527)
 */

#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

#ifdef __aarch64__

.text
.align 2

/* --- Macro para padronizar funções --- */
.macro aarch64_function fname
    .global \fname
    .type \fname, %function
\fname:
.endm

/* 
 * 1. tiled_to_planar (Plano Luma Y)
 * x0: SRC, x1: DST, x2: PITCH, x3: WIDTH, x4: HEIGHT 
 */
aarch64_function tiled_to_planar
    stp x19, x20, [sp, #-48]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]

    mov x19, x0 // SRC
    mov x20, x1 // DST
    mov x21, x2 // PITCH
    mov x22, x3 // WIDTH
    mov x23, x4 // HEIGHT

    // Tiles por linha = (width + 31) / 32
    add x7, x22, #31
    lsr x7, x7, #5

    mov x24, #0 // y_counter

1:  // Y-Loop
    cmp x24, x23
    b.ge 5f

    mov x9, #0 // x_counter

2:  // X-Loop
    cmp x9, x22
    b.ge 4f

    // Offset do Tile: (y/32 * tiles_per_row + x/32) * 1024 + (y%32 * 32)
    lsr x10, x24, #5
    mul x10, x10, x7
    lsr x11, x9, #5
    add x10, x10, x11
    lsl x10, x10, #10
    and x11, x24, #31
    lsl x11, x11, #5
    add x10, x10, x11
    add x10, x10, x19 // SRC Final

    // Destino: y * dst_pitch + x
    mul x11, x24, x21
    add x11, x11, x20
    add x11, x11, x9 // DST Final

    // Carga/Soma NEON 32-bytes
    ld1 {v0.16b, v1.16b}, [x10]
    st1 {v0.16b, v1.16b}, [x11]

    add x9, x9, #32
    b 2b

4:  add x24, x24, #1
    b 1b

5:  ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #48
    ret
.size tiled_to_planar, .-tiled_to_planar


/* 
 * 2. tiled_deinterleave_to_planar (Plano Chroma UV)
 * x0: SRC, x1: DST_U, x2: DST_V, x3: PITCH, x4: WIDTH, x5: HEIGHT
 */
aarch64_function tiled_deinterleave_to_planar
    stp x19, x20, [sp, #-64]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]

    mov x19, x0 // SRC
    mov x20, x1 // DST_U
    mov x21, x2 // DST_V
    mov x22, x3 // PITCH
    mov x23, x4 // WIDTH
    mov x24, x5 // HEIGHT

    // Tiles por linha
    add x7, x23, #31
    lsr x7, x7, #5

    mov x25, #0 // y_counter

1:  // Y-Loop
    cmp x25, x24
    b.ge 5f

    mov x9, #0 // x_counter

2:  // X-Loop
    cmp x9, x23
    b.ge 4f

    // Offset do Tile (Chroma tiles são 32x32 mas carregam U e V intercalados)
    lsr x10, x25, #5
    mul x10, x10, x7
    lsr x11, x9, #5
    add x10, x10, x11
    lsl x10, x10, #10
    and x11, x25, #31
    lsl x11, x11, #5
    add x10, x10, x19 // SRC Final

    // Destinos U e V: y * pitch + (x/2)
    mul x11, x25, x22
    lsr x12, x9, #1
    add x13, x11, x20 // Base U
    add x13, x13, x12 // U Final
    add x14, x11, x21 // Base V
    add x14, x14, x12 // V Final

    // ld2 desintercala os bytes: v0 (U), v1 (V)
    ld2 {v0.16b, v1.16b}, [x10]
    st1 {v0.16b}, [x13]
    st1 {v1.16b}, [x14]

    add x9, x9, #32
    b 2b

4:  add x25, x25, #1
    b 1b

5:  ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #64
    ret
.size tiled_deinterleave_to_planar, .-tiled_deinterleave_to_planar

#endif
