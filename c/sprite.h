/*
 * sprite.h - M3: wrap a GPU-rendered buffer as a RISC OS sprite.
 *
 * Format transcribed from ROOL's own PRM pages, not guessed - see
 * sprite.c for the exact sources and the byte-level derivation.
 */
#ifndef SPRITE_H
#define SPRITE_H

/* Sprite area header (16) + sprite header (44) = 60 bytes precede the
 * pixel data in every buffer this module writes a sprite into. */
#define SPRITE_HEADER_BYTES 60u

/*
 * Writes a valid one-sprite RISC OS sprite area (area header + sprite
 * header, no palette - 32bpp needs none) into the first
 * SPRITE_HEADER_BYTES of `buf`, describing a `width` x `height` 32bpp
 * sprite named `name` (truncated/padded to 12 bytes) whose pixel data
 * occupies the next `width * height * 4` bytes immediately after
 * (i.e. starting at `buf + SPRITE_HEADER_BYTES`). Does not touch the
 * pixel data itself - the caller (or, here, the GPU) fills that in
 * separately.
 */
void sprite_write_header(void *buf, unsigned int width, unsigned int height,
                          const char *name);

#endif /* SPRITE_H */
