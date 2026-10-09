/*
 * cl_bands.h - the arithmetic of splitting a big fill or copy into bands (0.12).
 *
 * Pure C with no kernel dependencies, so the driver (cl_fill.c, cl_copy.c) and the host
 * tests (test/host/) run the SAME code. See v3d_band_height in cl_common.h for why bands
 * exist: a GPU job holds at most 510 tiles, so a taller rectangle is cut into strips of
 * band_h pixel rows (band_h a multiple of 64), each strip its own job. The last strip is
 * whatever is left and may be shorter.
 */
#ifndef CL_BANDS_H
#define CL_BANDS_H

/* How many bands a rectangle `height` rows tall needs when each is band_h rows (band_h > 0). */
static inline unsigned int v3d_band_count(unsigned int height, unsigned int band_h)
{
    return (height + band_h - 1u) / band_h;
}

/* Band number `idx` (0 = topmost, idx < v3d_band_count): *y0 = its first row, *h = its height. */
static inline void v3d_band_span(unsigned int height, unsigned int band_h,
                                 unsigned int idx, unsigned int *y0, unsigned int *h)
{
    unsigned int top = idx * band_h;
    unsigned int left = height - top;

    *y0 = top;
    *h = left > band_h ? band_h : left;
}

#endif /* CL_BANDS_H */
