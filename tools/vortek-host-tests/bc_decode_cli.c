/*
 * Host test helper: decodes raw BCn blocks with Vortek's bc_decoder.h (the code shipped in the APK)
 * and writes the decoded texels. Used by test_bc_reference.py, which compares the output with an
 * independent decoder (Pillow's BCn decoder).
 *
 * usage: bc_decode_cli <bcN> <isNoAlphaU 0|1> <width> <height> <threads> <in.bin> <out.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "winlator.h"
#include "thread_pool.h"
#include "bc_decoder.h"

int main(int argc, char** argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: %s bcN isNoAlphaU width height threads in out\n", argv[0]);
        return 2;
    }

    int bcN = atoi(argv[1]);
    bool isNoAlphaU = atoi(argv[2]) != 0;
    int width = atoi(argv[3]);
    int height = atoi(argv[4]);
    int threads = atoi(argv[5]);

    FILE* in = fopen(argv[6], "rb");
    if (!in) return 3;
    fseek(in, 0, SEEK_END);
    long inSize = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t* src = malloc(inSize);
    if (fread(src, 1, inSize, in) != (size_t)inSize) return 4;
    fclose(in);

    size_t outSize = (size_t)width * height * BCDecoder_getOutputTexelSize(bcN);
    uint8_t* dst = calloc(1, outSize);

    ThreadPool* pool = threads > 0 ? ThreadPool_init(threads) : NULL;
    BCDecoder_decode(src, dst, width, height, bcN, isNoAlphaU, pool);

    FILE* out = fopen(argv[7], "wb");
    if (!out) return 5;
    fwrite(dst, 1, outSize, out);
    fclose(out);
    free(src);
    free(dst);
    return 0;
}
