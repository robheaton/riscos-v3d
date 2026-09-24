/*
 * sprite.c - see sprite.h.
 *
 * Every field here is transcribed from ROOL's own PRM pages, not
 * guessed - unlike the V3D control-list work, this is old, stable,
 * decades-unchanged RISC OS API, but the same "verify, don't infer
 * from memory" discipline applies:
 *   - Format Of Sprite Area: riscosopen.org/wiki/documentation/show/Format%20Of%20Sprite%20Area
 *   - Format Of Sprite:      riscosopen.org/wiki/documentation/show/Format%20Of%20Sprite
 *   - Sprite Mode Word:      riscosopen.org/wiki/documentation/show/Sprite%20Mode%20Word
 *
 * Unlike the V3D packets in cl_builder.h/cl_clear.c, these headers are
 * plain 32-bit words with no bitfields crossing byte boundaries - a
 * straightforward struct of `unsigned int`s (name[12] included, since
 * 12 is a multiple of 4) lays out identically to the documented format
 * under normal ARM/AAPCS alignment rules, so there's no need for the
 * explicit-shift byte-packing cl_builder.h uses for the V3D side.
 */

#include "sprite.h"

typedef struct sprite_area_header
{
    unsigned int size;   /* +0: size of sprite area, including this header */
    unsigned int count;  /* +4: number of sprites in area */
    unsigned int first;  /* +8: offset to first sprite (16, right after this header) */
    unsigned int free;   /* +12: offset to first free word (= size, area is full) */
} sprite_area_header_t;

typedef struct sprite_header
{
    unsigned int next;             /* +0: offset to next sprite (0 = none after this one,
                                     * but PRM says "last sprite: next = size of this
                                     * sprite" - see sprite_write_header) */
    char name[12];                 /* +4 */
    unsigned int width_words_m1;   /* +16: sprite width in words, minus 1 */
    unsigned int height_m1;        /* +20: sprite height in pixels, minus 1 */
    unsigned int first_bit_used;   /* +24: 0 for new-format sprites */
    unsigned int last_bit_used;    /* +28: last used bit of last word (31 for 32bpp) */
    unsigned int image_offset;     /* +32: bytes from this header to pixel data */
    unsigned int mask_offset;      /* +36: = image_offset (no mask) */
    unsigned int mode_word;        /* +40 */
} sprite_header_t;

/* Portable compile-time assertion (no <assert.h> static_assert reliance):
 * fails to compile if natural ARM struct alignment ever doesn't match
 * the PRM's documented byte sizes exactly. */
typedef char sprite_area_header_size_check[(sizeof(sprite_area_header_t) == 16) ? 1 : -1];
typedef char sprite_header_size_check[(sizeof(sprite_header_t) == 44) ? 1 : -1];

/*
 * RISC OS 3.5-style sprite mode word (see Sprite Mode Word doc):
 *   bit 0     = 1 (always)
 *   bits 1-13 = horizontal DPI
 *   bits 14-26 = vertical DPI
 *   bits 27-30 = sprite type (6 = 32bpp 8:8:8:8 TBGR)
 *   bit 31    = wide mask flag (0: no mask here)
 * 90 DPI (eigen value 1) chosen for both axes - one of the four DPI
 * values the PRM notes are guaranteed expressible as an eigen value,
 * which matters for some OS APIs.
 */
#define SPRITE_TYPE_32BPP 6u
#define SPRITE_DPI        90u

static unsigned int sprite_mode_word(void)
{
    return 1u | (SPRITE_DPI << 1) | (SPRITE_DPI << 14) | (SPRITE_TYPE_32BPP << 27);
}

void sprite_write_header(void *buf, unsigned int width, unsigned int height,
                          const char *name)
{
    sprite_area_header_t *area = (sprite_area_header_t *) buf;
    sprite_header_t *spr =
        (sprite_header_t *) ((unsigned char *) buf + sizeof(sprite_area_header_t));
    unsigned int pixel_bytes = width * height * 4u;
    unsigned int sprite_bytes = sizeof(sprite_header_t) + pixel_bytes;
    unsigned int i;

    area->size = sizeof(sprite_area_header_t) + sprite_bytes;
    area->count = 1;
    area->first = sizeof(sprite_area_header_t);
    area->free = area->size;

    spr->next = sprite_bytes; /* last (only) sprite: offset to one-past-end */
    for (i = 0; i < sizeof(spr->name) && name[i] != '\0'; i++)
        spr->name[i] = name[i];
    for (; i < sizeof(spr->name); i++)
        spr->name[i] = '\0'; /* PRM: name is null-padded */

    spr->width_words_m1 = (width * 4u) / 4u - 1u; /* 32bpp: 1 pixel = 1 word */
    spr->height_m1 = height - 1u;
    spr->first_bit_used = 0;
    spr->last_bit_used = 31;
    spr->image_offset = sizeof(sprite_header_t);
    spr->mask_offset = sizeof(sprite_header_t); /* no mask */
    spr->mode_word = sprite_mode_word();
}
