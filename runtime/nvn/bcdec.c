// BC1-BC7 -> texels (bcdec.h).
#include "bcdec.h"

#include <pthread.h>
#include <string.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE  // (the signed formats: normal maps)
#include "../../third_party/bcdec/bcdec.h"

uint32_t bc_texel_size(int kind) {
    switch (kind) {
    case BC_4U: case BC_4S: return 1;
    case BC_5U: case BC_5S: return 2;
    case BC_6HU: case BC_6HS: return 8;
    default: return 4;
    }
}

uint32_t bc_block_size(int kind) { return kind == BC_1 || kind == BC_4U || kind == BC_4S ? 8 : 16; }

// One block into a 4x4 scratch of the output texels, then the part of it inside the image.
static void decode_rows(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out, uint32_t by0, uint32_t by1) {
    uint32_t wb = (width + 3) / 4, ts = bc_texel_size(kind), bs = bc_block_size(kind);
    for (uint32_t by = by0; by < by1; by++) {
        uint32_t rows = height - by * 4 < 4 ? height - by * 4 : 4;
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *src = blocks + ((uint64_t)by * wb + bx) * bs;
            uint8_t px[4 * 4 * 8];
            switch (kind) {
            case BC_1: bcdec_bc1(src, px, 16); break;
            case BC_2: bcdec_bc2(src, px, 16); break;
            case BC_3: bcdec_bc3(src, px, 16); break;
            case BC_7: bcdec_bc7(src, px, 16); break;
            case BC_4U: case BC_4S: bcdec_bc4(src, px, 4, kind == BC_4S); break;
            case BC_5U: case BC_5S: bcdec_bc5(src, px, 8, kind == BC_5S); break;
            default: {  // BC6H: RGB halves -> RGBA16F
                uint16_t rgb[4 * 4 * 3], *o = (uint16_t *)(void *)px;
                bcdec_bc6h_half(src, rgb, 12, kind == BC_6HS);
                for (int i = 0; i < 16; i++) {
                    o[i * 4] = rgb[i * 3]; o[i * 4 + 1] = rgb[i * 3 + 1]; o[i * 4 + 2] = rgb[i * 3 + 2];
                    o[i * 4 + 3] = 0x3C00;  // 1.0
                }
            }
            }
            uint32_t cols = width - bx * 4 < 4 ? width - bx * 4 : 4;
            for (uint32_t y = 0; y < rows; y++)
                memcpy(out + (((uint64_t)by * 4 + y) * width + (uint64_t)bx * 4) * ts, px + y * 4 * ts, cols * ts);
        }
    }
}

typedef struct { int kind; const uint8_t *blocks; uint32_t width, height; uint8_t *out; uint32_t by0, by1; } Job;
static void *job_run(void *arg) {
    Job *j = arg;
    decode_rows(j->kind, j->blocks, j->width, j->height, j->out, j->by0, j->by1);
    return NULL;
}

// Large levels are split over a few threads (a 2048x2048 BC7 level is 262144 blocks: tens of milliseconds on one
// core, in the middle of a frame); the small ones, most of them, aren't worth starting any.
void bc_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out) {
    uint32_t wb = (width + 3) / 4, hb = (height + 3) / 4;
    enum { MAXT = 4 };
    uint32_t nt = (uint64_t)wb * hb >= 16384 ? MAXT : 1;
    if (nt > hb) nt = hb ? hb : 1;
    if (nt <= 1) { decode_rows(kind, blocks, width, height, out, 0, hb); return; }
    Job jobs[MAXT];
    pthread_t th[MAXT];
    bool started[MAXT] = {false};
    for (uint32_t i = 0; i < nt; i++) {
        jobs[i] = (Job){kind, blocks, width, height, out, hb * i / nt, hb * (i + 1) / nt};
        if (i + 1 < nt) started[i] = pthread_create(&th[i], NULL, job_run, &jobs[i]) == 0;
        if (!started[i]) job_run(&jobs[i]);  // (the last part, and any whose thread didn't start: here)
    }
    for (uint32_t i = 0; i < nt; i++)
        if (started[i]) pthread_join(th[i], NULL);
}
