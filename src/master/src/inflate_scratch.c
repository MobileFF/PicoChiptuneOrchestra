#include "inflate_scratch.h"

static tinfl_decompressor s_decomp;
static uint8_t s_dict[TINFL_LZ_DICT_SIZE];

tinfl_decompressor *inflate_scratch_decomp(void) { return &s_decomp; }
uint8_t *inflate_scratch_dict(void) { return s_dict; }
