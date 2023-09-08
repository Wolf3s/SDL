#include "SDL_internal.h"

#if SDL_HAVE_BLIT_A

#ifdef SDL_AVX2_INTRINSICS

#define SDL_blit_A_avx2_c

#include "SDL_blit.h"
#include "SDL_blit_A_sse4_1.h"

__m256i SDL_TARGETING("avx2") GetSDL_PixelFormatAlphaMask_AVX2(const SDL_PixelFormat* dstfmt) {
    Uint8 index = dstfmt->Ashift / 4;
    /* Handle case where bad input sent */
    if (dstfmt->Ashift == dstfmt->Bshift && dstfmt->Ashift == 0) {
        index = 6;
    }
    return _mm256_set_epi8(
            -1, index + 24, -1, index + 24, -1, index + 24, -1, index + 24,
            -1, index + 16, -1, index + 16, -1, index + 16, -1, index + 16,
            -1, index + 8, -1, index + 8, -1, index + 8, -1, index + 8,
            -1, index, -1, index, -1, index, -1, index);
}

/**
 * Using the AVX2 instruction set, blit eight pixels with alpha blending
 * @param src A pointer to four 32-bit pixels of ARGB format to blit into dst
 * @param dst A pointer to four 32-bit pixels of ARGB format to retain visual data for while alpha blending
 * @return A 128-bit wide vector of four alpha-blended pixels in ARGB format
 */
__m128i SDL_TARGETING("avx2") MixRGBA_AVX2(const __m128i src, const __m128i dst, const __m256i alphaMask) {
    __m256i src_color = _mm256_cvtepu8_epi16(src);
    __m256i dst_color = _mm256_cvtepu8_epi16(dst);
    __m256i alpha = _mm256_shuffle_epi8(src_color, alphaMask);
    __m256i sub = _mm256_sub_epi16(src_color, dst_color);
    __m256i mul = _mm256_mullo_epi16(sub, alpha);
    /**
     * With an 8-bit shuffle, one can only move integers within a lane. The 256-bit AVX2 lane is actually 4 64-bit
     * lanes. We pack the integers into the start of each lane. The second shuffle operates on these 64-bit integers to
     * put them into the correct order for transport back to the surface in the correct format.
     */
    const __m256i SHUFFLE_REDUCE = _mm256_set_epi8(
            -1, -1, -1, -1, -1, -1, -1, -1,
            31, 29, 27, 25, 23, 21, 19, 17,
            -1, -1, -1, -1, -1, -1, -1, -1,
            15, 13, 11, 9, 7, 5, 3, 1);
    __m256i reduced = _mm256_shuffle_epi8(mul, SHUFFLE_REDUCE);
    __m256i packed = _mm256_permute4x64_epi64(reduced, _MM_SHUFFLE(3, 1, 2, 0));
    __m128i mix = _mm256_castsi256_si128(packed);
    return _mm_add_epi8(mix, dst);
}

void SDL_TARGETING("avx2") BlitNtoNPixelAlpha_AVX2(SDL_BlitInfo *info)
{
    int width = info->dst_w;
    int height = info->dst_h;
    Uint8 *src = info->src;
    int srcskip = info->src_skip;
    Uint8 *dst = info->dst;
    int dstskip = info->dst_skip;
    SDL_PixelFormat *srcfmt = info->src_fmt;
    SDL_PixelFormat *dstfmt = info->dst_fmt;

    int chunks = width / 4;
    const __m128i colorShiftMask = GetSDL_PixelFormatShuffleMask(srcfmt, dstfmt);
    const __m256i alphaMask = GetSDL_PixelFormatAlphaMask_AVX2(dstfmt);
    const __m128i sse4_1AlphaMask = GetSDL_PixelFormatAlphaMask_SSE4_1(dstfmt);

    while (height--) {
        /* Process 4-wide chunks of source color data that may be in wrong format */
        for (int i = 0; i < chunks; i += 1) {
            __m128i c_src = _mm_shuffle_epi8(_mm_loadu_si128((__m128i *) (src + i * 16)), colorShiftMask);
            /* Alpha-blend in 4-wide chunk from src into destination */
            __m128i c_dst = _mm_loadu_si128((__m128i*) (dst + i * 16));
            __m128i c_mix = MixRGBA_AVX2(c_src, c_dst, alphaMask);
            _mm_storeu_si128((__m128i*) (dst + i * 16), c_mix);
        }

        /* Handle remaining pixels when width is not a multiple of 4 */
        if (width % 4 != 0) {
            int remaining_pixels = width % 4;
            int offset = width - remaining_pixels;
            if (remaining_pixels >= 2) {
                Uint32 *src_ptr = ((Uint32*)(src + (offset * 4)));
                Uint32 *dst_ptr = ((Uint32*)(dst + (offset * 4)));
                __m128i c_src = _mm_loadu_si64(src_ptr);
                c_src = _mm_unpacklo_epi8(_mm_shuffle_epi8(c_src, colorShiftMask), _mm_setzero_si128());
                __m128i c_dst = _mm_unpacklo_epi8(_mm_loadu_si64(dst_ptr), _mm_setzero_si128());
                __m128i c_mix = MixRGBA_SSE4_1(c_src, c_dst, sse4_1AlphaMask);
                c_mix = _mm_packus_epi16(c_mix, _mm_setzero_si128());
                _mm_storeu_si64(dst_ptr, c_mix);
                remaining_pixels -= 2;
                offset += 2;
            }
            if (remaining_pixels == 1) {
                Uint32 *src_ptr = ((Uint32*)(src + (offset * 4)));
                Uint32 *dst_ptr = ((Uint32*)(dst + (offset * 4)));
                Uint32 pixel = AlignPixelToSDL_PixelFormat(*src_ptr, srcfmt, dstfmt);
                /* Old GCC has bad or no _mm_loadu_si32 */
                #if defined(__GNUC__) && (__GNUC__ < 11)
                __m128i c_src = _mm_set_epi32(0, 0, 0, pixel);
                __m128i c_dst = _mm_set_epi32(0, 0, 0, *dst_ptr);
                #else
                __m128i c_src = _mm_loadu_si32(&pixel);
                __m128i c_dst = _mm_loadu_si32(dst_ptr);
                #endif
                c_src = _mm_unpacklo_epi8(c_src, _mm_setzero_si128());
                c_dst = _mm_unpacklo_epi8(c_dst, _mm_setzero_si128());
                __m128i mixed_pixel = MixRGBA_SSE4_1(c_src, c_dst, sse4_1AlphaMask);
                mixed_pixel = _mm_srli_epi16(mixed_pixel, 8);
                mixed_pixel = _mm_unpacklo_epi8(mixed_pixel, _mm_setzero_si128());
                /* Old GCC has bad or no _mm_storeu_si32 */
                #if defined(__GNUC__) && (__GNUC__ < 11)
                *dst_ptr = _mm_extract_epi32(mixed_pixel, 0);
                #else
                _mm_storeu_si32(dst_ptr, mixed_pixel);
                #endif
            }
        }

        src += 4 * width;
        dst += 4 * width;

        src += srcskip;
        dst += dstskip;
    }
}

#endif

#endif
