/* cf_inflate.h: a small raw DEFLATE decoder (cf_inflate.c) for SGZ1 blocks, its output in segments (lane gamecomp1) */
#ifndef CF_INFLATE_H
#define CF_INFLATE_H
#include <stdint.h>

#define CF_INFL_IN 1024u
#define CF_INFL_LFAST 9                   /* lookup bits: literal / length codes, distance codes */
#define CF_INFL_DFAST 7
typedef struct {
    uint16_t lcount[16], lsym[288], dcount[16], dsym[32];
    uint16_t lfast[1u << CF_INFL_LFAST], dfast[1u << CF_INFL_DFAST];   /* (code length << 12) | symbol, 0 = longer code */
    int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n);
    void *u;
    uint32_t src_off, src_len, src_done, in_n, in_pos, bitbuf, bitcnt;
    uint8_t *const *seg;
    uint8_t seg_shift;
    uint32_t out_len, out_pos;
    uint8_t in[CF_INFL_IN];
} cf_infl_t;

/* src_len bytes of one raw DEFLATE stream at src_off (through rd) -> exactly out_len bytes into seg[i] (each
   1 << seg_shift bytes). 0 = done with every input byte used; negative = refused (any malformed stream, too much or
   too little output, a reference before the start, bytes left over). */
int cf_inflate(cf_infl_t *s, int (*rd)(void *u, uint32_t off, uint8_t *b, uint32_t n), void *u, uint32_t src_off, uint32_t src_len,
               uint8_t *const *seg, uint8_t seg_shift, uint32_t out_len);
#endif
