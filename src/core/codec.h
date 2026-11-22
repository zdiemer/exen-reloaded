/* In-Fusio packed-data codecs and the "ExEn PNG" image container.
 *
 * Ported from the 2003 ExEn simulator (vendor/gensimu/gensimu.exe). The algorithms, addresses
 * and validation results are documented in docs/re/CODECS.md. Portable C11: no OS headers, and
 * the only allocations are malloc/free calls for the decoded image (freed by
 * exen_image_free).
 *
 * Packed stream header (all five codecs, big-endian bit order, MSB first):
 *   bits 0..3    codec number (first byte >> 4): 1..5
 *   bits 4..35   A = row width in bits
 *   bits 36..67  B = row count
 *   The stream holds ((A + 7) & ~7) * B bits, i.e. that many bits / 8 bytes once unpacked.
 *   Codec 5 also stores the unpacked byte count as a 24-bit little-endian value at bytes 9..11
 *   and uses that one instead of A and B.
 *
 * Type-0x0d image resources and type-0x81 packed resources (Resource.readBytes) both use these
 * codecs. Type 0x01/0x81 resources are 1-bit bitmaps for Resource.readImage (framework bytecode,
 * exen.Resource @0x1cc): `u16 w, u16 h`, then readBytes(((w + 7) >> 3) * h), which unpacks a
 * 0x81 resource. So a 0x81 resource's packed stream starts at offset 4.
 */
#ifndef EXEN_CODEC_H
#define EXEN_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* Return codes. 0 is success; everything else is negative. */
enum {
    EXEN_CODEC_OK = 0,
    EXEN_CODEC_ERR_ARG = -1,       /* NULL pointer or similar */
    EXEN_CODEC_ERR_METHOD = -2,    /* first nibble is not 1..5 (the simulator returns NULL) */
    EXEN_CODEC_ERR_SHORT = -3,     /* input shorter than the header */
    EXEN_CODEC_ERR_SIZE = -4,      /* dst_cap and the stream size disagree (see exen_unpack) */
    EXEN_CODEC_ERR_CORRUPT = -5,   /* stream reads far past its end (the simulator would hang) */
    EXEN_CODEC_ERR_NOMEM = -6,
    EXEN_CODEC_ERR_FORMAT = -7,    /* image: not an ExEn PNG, or no IHDR/IDAT */
    EXEN_CODEC_ERR_UNSUPPORTED = -8 /* image: colour type other than 3 (indexed) */
};

/* Codec number (1..5) of a packed stream, or EXEN_CODEC_ERR_* if it has none. */
int exen_unpack_method(const uint8_t *src, size_t src_len);

/* Unpacked size in bytes declared by a packed stream's header: bytes 9..11 for codec 5,
 * ((A + 7) & ~7) * B / 8 for codecs 1..4. Returns 0 and sets *size on success. */
int exen_unpacked_size(const uint8_t *src, size_t src_len, size_t *size);

/* Unpack src into dst, dispatching on the first nibble the way codecCreate (0x432e50) does.
 *
 * dst_cap plays the part of the simulator's outSize argument: codecs 1..4 produce exactly
 * dst_cap bytes (or fewer when the stream ends first), as the simulator does when the bytecode
 * asks readBytes(n) or the image decoder asks for pitch * height bytes. Codec 5 ignores the
 * requested size and always produces the size at bytes 9..11; if that exceeds dst_cap, the
 * output is cut at dst_cap and EXEN_CODEC_ERR_SIZE is returned (the simulator would overrun).
 * Codecs 2 and 4 refuse (write nothing, EXEN_CODEC_ERR_SIZE) when dst_cap is larger than the
 * stream, again as the simulator does.
 *
 * *out_len (may be NULL) receives the number of bytes written. */
int exen_unpack(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len);

/* A decoded image.
 *
 * A type-0x0d resource is `u16 width, u16 height, u16 stream_len` (little-endian) followed by a
 * PNG signature and PNG-style chunks `[u32 BE length][4cc][data]` without CRCs. IHDR, PLTE and
 * tRNS have their PNG meaning; IDAT is one packed stream (see above), not zlib, and has no
 * per-row filter bytes. Decoding stops at the first IDAT, as in the simulator (0x41dacd).
 * Without the PNG signature the simulator reads ((w + 7) >> 3) * h bytes after the 6-byte header
 * as a raw 1-bit bitmap (decodeBitmap 0x418d0a); so does exen_image_decode (codec = 0). */
typedef struct ExenDecodedImage {
    uint32_t width, height;     /* from IHDR */
    uint16_t res_width;         /* resource header u16 #0 (= width in the whole corpus) */
    uint16_t res_height;        /* resource header u16 #1 (= height in the whole corpus) */
    uint16_t res_stream_len;    /* resource header u16 #2: byte length of the PNG stream */
    uint8_t bit_depth;          /* IHDR bit depth: 4 or 8 in the corpus */
    uint8_t color_type;         /* IHDR colour type: always 3 (indexed) */
    uint8_t codec;              /* IDAT codec number 1..5; 0 for a raw 1-bit bitmap */
    /* pixels[y * width + x] = palette index, one byte per pixel. For bit depth 4 the packed
     * rows are (width + 1) / 2 bytes with the left pixel in the high nibble; they are expanded
     * here. For bit depths 1 and 2 the simulator unpacks one byte per pixel (outSize = w * h)
     * and so does this decoder; no game uses them. 1-bit bitmaps (codec 0): rows of
     * (width + 7) / 8 bytes, MSB = left pixel, expanded to 0/1, palette {black, white}. */
    uint8_t *pixels;
    /* The packed IDAT output exactly as the simulator holds it with keepPalette = true:
     * pitch * height bytes, pitch = (width + 1) / 2 for depth 4, else width. */
    uint8_t *raw;
    uint32_t pitch;
    size_t raw_len;
    /* PLTE as RGB triplets. palette_count = PLTE length / 3 (entries past 256 are dropped). */
    uint8_t palette[256 * 3];
    uint16_t palette_count;
    /* tRNS for colour type 3: alpha[i] for palette entry i, alpha_count = tRNS length (entries
     * past 256 dropped). Entries >= alpha_count are opaque (255); alpha is pre-filled with 255.
     * has_trns tells whether a tRNS chunk was present. */
    uint8_t alpha[256];
    uint16_t alpha_count;
    uint8_t has_trns;
    /* The colour key the simulator uses: the LAST palette index whose tRNS alpha is 0, or -1.
     * (keepPalette = true sets transpIdx to it; keepPalette = false keys on its RGB332 value.)
     * transparent_zero_count = how many entries have alpha 0; with keepPalette the simulator
     * uses colour-key mode 0x50 for exactly one and per-entry alpha mode 0x30 for two or more. */
    int transparent_index;
    uint16_t transparent_zero_count;
} ExenDecodedImage;

/* Decode a type-0x0d resource (starting at the u16 width). Returns 0 on success, negative
 * EXEN_CODEC_ERR_* otherwise. On success, release with exen_image_free. On failure nothing
 * needs freeing. */
int exen_image_decode(const uint8_t *res, size_t len, ExenDecodedImage *out);

/* Build an image from a 1-bit bitmap of `width` x `height` (rows of (width + 7) / 8 bytes, MSB
 * first, 1 = white), as the simulator's import1bpp (0x42bdb3) does with keepPalette = true:
 * index 0 = black, 1 = white. Used for raw type-0x0d bitmaps and for unpacked 0x01/0x81
 * resources. Returns EXEN_CODEC_ERR_SHORT if len is too small. */
int exen_image_from_1bpp(const uint8_t *bits, size_t len, uint32_t width, uint32_t height,
                         ExenDecodedImage *out);

void exen_image_free(ExenDecodedImage *img);

#endif
