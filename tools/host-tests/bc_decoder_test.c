#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
typedef struct ThreadPool { int numThreads; } ThreadPool;
static void ThreadPool_run(ThreadPool* p, void (*f)(void*), void* a) { f(a); }
static void ThreadPool_wait(ThreadPool* p) {}
#include "bc_decoder.h"
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint64_t bc4block(uint8_t e0, uint8_t e1, int idx) { uint64_t b = e0 | ((uint64_t)e1 << 8); for (int i = 0; i < 16; i++) b |= (uint64_t)(idx & 7) << (16 + 3 * i); return b; }
int main(void) {
  // BC4 UNORM 4x4, all texels = endpoint0 (idx 0) = 200 -> 16 bytes of 200 (R8)
  { uint64_t blk = bc4block(200, 50, 0); uint8_t out[16 + 8]; memset(out, 0xEE, sizeof out);
    BCDecoder_decode((uint8_t*)&blk, out, 4, 4, 4, true, NULL);
    for (int i = 0; i < 16; i++) CHECK(out[i] == 200, "bc4 unorm texel %d = %d", i, out[i]);
    for (int i = 16; i < 24; i++) CHECK(out[i] == 0xEE, "bc4 wrote past end at %d", i); }
  // BC4 SNORM: e0=-100 (0x9C), e1=100 -> e0<e1 (6-value mode); idx 0 -> -100 -> byte 0x9C (R8_SNORM)
  { uint64_t blk = bc4block((uint8_t)(int8_t)-100, 100, 0); uint8_t out[16];
    BCDecoder_decode((uint8_t*)&blk, out, 4, 4, 4, false, NULL);
    for (int i = 0; i < 16; i++) CHECK((int8_t)out[i] == -100, "bc4 snorm texel %d = %d", i, (int8_t)out[i]); }
  // BC5 UNORM: red=10, green=240 -> RG8 pairs
  { uint64_t blk[2] = { bc4block(10, 0, 0), bc4block(240, 0, 0) }; uint8_t out[32 + 4]; memset(out, 0xEE, sizeof out);
    BCDecoder_decode((uint8_t*)blk, out, 4, 4, 5, true, NULL);
    for (int i = 0; i < 16; i++) { CHECK(out[2*i] == 10, "bc5 R texel %d = %d", i, out[2*i]); CHECK(out[2*i+1] == 240, "bc5 G texel %d = %d", i, out[2*i+1]); }
    for (int i = 32; i < 36; i++) CHECK(out[i] == 0xEE, "bc5 wrote past end"); }
  // BC1: c0 = pure red (0xF800) > c1 = blue (0x001F), idx 0 -> BGRA (0,0,255,255)
  { uint16_t blk[4] = { 0xF800, 0x001F, 0, 0 }; uint8_t out[64];
    BCDecoder_decode((uint8_t*)blk, out, 4, 4, 1, true, NULL);
    for (int i = 0; i < 16; i++) CHECK(out[4*i]==0 && out[4*i+1]==0 && out[4*i+2]==255 && out[4*i+3]==255, "bc1 texel %d = %d,%d,%d,%d", i, out[4*i], out[4*i+1], out[4*i+2], out[4*i+3]); }
  // BC4 6x6 (2x2 blocks, partial edges): output exactly 36 bytes
  { uint64_t blk[4] = { bc4block(1,0,0), bc4block(2,0,0), bc4block(3,0,0), bc4block(4,0,0) }; uint8_t* out = malloc(36);
    BCDecoder_decode((uint8_t*)blk, out, 6, 6, 4, true, NULL);
    CHECK(out[0]==1 && out[4]==2 && out[6*4]==3 && out[6*4+4]==4 && out[35]==4, "bc4 6x6 layout %d %d %d %d %d", out[0], out[4], out[24], out[28], out[35]);
    free(out); }
  // BC3 alpha channel lands in byte 3 with 4-byte texels
  { uint64_t blk[2] = { bc4block(77, 0, 0), 0 }; uint16_t* col = (uint16_t*)&blk[1]; col[0] = 0xFFFF; col[1] = 0; uint8_t out[64];
    BCDecoder_decode((uint8_t*)blk, out, 4, 4, 3, false, NULL);
    for (int i = 0; i < 16; i++) CHECK(out[4*i+3] == 77 && out[4*i+2] == 255, "bc3 texel %d a=%d r=%d", i, out[4*i+3], out[4*i+2]); }
  printf(fails ? "%d FAILURES\n" : "ALL BC DECODER TESTS PASSED\n", fails);
  return fails != 0;
}
