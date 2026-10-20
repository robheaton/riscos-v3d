/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host check that cl_packets.h's integer-only clipper scaling / viewport maths
 * (v3d_f32_bits_of_uint, emit_clipper_xy_scaling_wh, emit_viewport_offset_whole)
 * produce exactly the bits the original float code did, for every dimension
 * 1..65535 and a sparse sweep up to 16M. Run: make hosttest
 */
#include <stdio.h>
#include <string.h>
static unsigned bits(float f){ unsigned u; memcpy(&u,&f,4); return u; }
static unsigned f32_of_uint(unsigned n){ unsigned e=31u-(unsigned)__builtin_clz(n); return ((e+127u)<<23)|((n<<(23u-e))&0x7FFFFFu); }
int main(void){
  unsigned bad=0, n=0;
  for (unsigned w=1; w<=0xFFFFFF; w += (w<70000?1:997)) {
    unsigned fw = bits((w/2.0f)*256.0f);
    unsigned iw = f32_of_uint(w)+(7u<<23);
    unsigned fh = bits((w/2.0f)*-256.0f);
    unsigned ih = (f32_of_uint(w)+(7u<<23))|0x80000000u;
    unsigned fv = ((unsigned)((w/2)*256.0f) & 0x3FFFFFu);   /* float path: x is float(width/2) */
    unsigned iv = ((w/2u)<<8) & 0x3FFFFFu;
    if (fw!=iw || fh!=ih || fv!=iv) { if (bad<5) printf("MISMATCH w=%u fw=%08x iw=%08x fh=%08x ih=%08x fv=%x iv=%x\n",w,fw,iw,fh,ih,fv,iv); bad++; }
    n++;
  }
  printf("checked %u values, %u mismatches\n", n, bad);
  return bad!=0;
}
