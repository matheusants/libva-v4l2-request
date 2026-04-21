#include <stdint.h>
#include <string.h>

void tiled_to_planar(uint8_t *src, uint8_t *dst, uint32_t dst_pitch, 
                      uint32_t width, uint32_t height) {
    uint32_t stride = (width + 31) & ~31;
    uint32_t tiles_per_row = stride / 32;
    
    // O pulo do gato: detectar se estamos no plano Y (32x32) ou UV (32x16)
    // Se a altura for menor que o esperado para o tile padrão, ajustamos.
    uint32_t tile_h = (height % 32 == 0 && height >= 32) ? 32 : 16;
    uint32_t tile_size = 32 * tile_h; 

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tile_y_idx = y / tile_h;
        uint32_t in_tile_y  = y % tile_h;
        uint8_t *d_ptr = dst + (y * dst_pitch);

        for (uint32_t tx = 0; tx < tiles_per_row; tx++) {
            uint32_t x_pos = tx * 32;
            if (x_pos >= width) continue;

            uint8_t *tile_base = src + (tile_y_idx * tiles_per_row + tx) * tile_size;
            uint8_t *s_ptr   = tile_base + (in_tile_y * 32);

            uint32_t copy_w = (width - x_pos < 32) ? (width - x_pos) : 32;
            memcpy(d_ptr + x_pos, s_ptr, copy_w);
        }
    }
}
