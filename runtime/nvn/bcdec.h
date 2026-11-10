// BC1-BC7 (S3TC / RGTC / BPTC) decoder to plain texels, for GPUs without those formats: mobile ones (Mali, older
// Adreno drivers) - the game's textures are almost all BC. The counterpart of astc.h, which covers the desktop GPUs'
// gap. The block decoders are third_party/bcdec (iOrange, MIT / Unlicense).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { BC_NONE, BC_1, BC_2, BC_3, BC_4U, BC_4S, BC_5U, BC_5S, BC_6HU, BC_6HS, BC_7 };

// Bytes of a decoded texel: 4 (RGBA8: BC1 / 2 / 3 / 7), 1 (R8: BC4), 2 (RG8: BC5), 8 (RGBA16F: BC6H).
uint32_t bc_texel_size(int kind);
// Bytes of a block: 8 (BC1, BC4) or 16.
uint32_t bc_block_size(int kind);
// blocks: row-major blocks covering ceil(width/4) x ceil(height/4). out: width*height texels, tightly packed rows
// (BC4S / BC5S: signed bytes, as the SNORM formats take them; BC6H: half floats, alpha 1).
void bc_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out);
