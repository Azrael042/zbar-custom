/*------------------------------------------------------------------------
 *  Copyright 2024 (c) ZBar Aztec Support
 *
 *  This file is part of the ZBar Bar Code Reader.
 *
 *  The ZBar Bar Code Reader is free software; you can redistribute it
 *  and/or modify it under the terms of the GNU Lesser Public License as
 *  published by the Free Software Foundation; either version 2.1 of
 *  the License, or (at your option) any later version.
 *
 *  The ZBar Bar Code Reader is distributed in the hope that it will be
 *  useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *  of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser Public License
 *  along with the ZBar Bar Code Reader; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 *  Boston, MA  02110-1301  USA
 *
 *  http://sourceforge.net/projects/zbar
 *------------------------------------------------------------------------*/

#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "aztec.h"
#include "img_scanner.h"
#include "symbol.h"

/* Define fourcc macro for creating format codes */
#define fourcc(a, b, c, d) ((unsigned long)((a) | ((b) << 8) | ((c) << 16) | ((d) << 24)))

/* Calculate image stride based on format */
static inline int get_image_stride(const zbar_image_t *img)
{
    unsigned width = zbar_image_get_width(img);
    unsigned long format = zbar_image_get_format(img);
    
    /* For grayscale images, stride equals width */
    if (format == fourcc('Y', '8', '0', '0') || 
        format == fourcc('G', 'R', 'E', 'Y')) {
        return width;
    }
    
    /* For RGB images, stride is width * 3 */
    if (format == fourcc('R', 'G', 'B', '3') ||
        format == fourcc('B', 'G', 'R', '3')) {
        return width * 3;
    }
    
    /* For RGBA images, stride is width * 4 */
    if (format == fourcc('R', 'G', 'B', 'A') ||
        format == fourcc('B', 'G', 'R', 'A')) {
        return width * 4;
    }
    
    /* Default to width for unknown formats */
    return width;
}

/* Aztec bull's-eye finder patterns */
static const int compact_bullseye_pattern[9] = {
    0x1FF, /* 111111111 */
    0x155, /* 101010101 */
    0x1FF, /* 111111111 */
    0x155, /* 101010101 */
    0x1FF, /* 111111111 */
    0x155, /* 101010101 */
    0x1FF, /* 111111111 */
    0x155, /* 101010101 */
    0x1FF  /* 111111111 */
};

static const int full_bullseye_pattern[13] = {
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF, /* 1111111111111 */
    0x1555, /* 1010101010101 */
    0x1FFF  /* 1111111111111 */
};

#define RS_FCR 1

/* Detect bull's-eye finder pattern */
static int detect_bullseye_pattern(const zbar_image_t *img, int x, int y, int *is_compact)
{
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    const unsigned char *data = zbar_image_get_data(img);
    int row_stride = get_image_stride(img);
    
    /* Check image bounds */
    if (x < AZTEC_MIN_QUIET_ZONE || x >= width - AZTEC_MIN_QUIET_ZONE ||
        y < AZTEC_MIN_QUIET_ZONE || y >= height - AZTEC_MIN_QUIET_ZONE) {
        return 0;
    }

    /* Check compact pattern */
    int match_compact = 1;
    for (int i = 0; i < AZTEC_COMPACT_FINDER && match_compact; i++) {
        const unsigned char *row = data + (y + i) * row_stride;
        int expected = compact_bullseye_pattern[i];
        int actual = 0;
        
        for (int j = 0; j < 9; j++) {
            actual = (actual << 1) | (row[x + j] > 127);
        }
        
        if (actual != expected) {
            match_compact = 0;
        }
    }

    if (match_compact) {
        *is_compact = 1;
        return AZTEC_COMPACT_FINDER;
    }

    /* Check full pattern */
    int match_full = 1;
    for (int i = 0; i < AZTEC_FULL_FINDER && match_full; i++) {
        const unsigned char *row = data + (y + i) * row_stride;
        int expected = full_bullseye_pattern[i];
        int actual = 0;
        
        for (int j = 0; j < 13; j++) {
            actual = (actual << 1) | (row[x + j] > 127);
        }
        
        if (actual != expected) {
            match_full = 0;
        }
    }

    if (match_full) {
        *is_compact = 0;
        return AZTEC_FULL_FINDER;
    }

    return 0;
}

/* Determine orientation from corner patterns */
static int determine_orientation(const zbar_image_t *img, aztec_finder_t *finder)
{
    const unsigned char *data = zbar_image_get_data(img);
    int row_stride = get_image_stride(img);
    int x = finder->center_x;
    int y = finder->center_y;
    int size = finder->is_compact ? AZTEC_COMPACT_CORE : AZTEC_FULL_CORE;
    int corners[4] = {0, 0, 0, 0};

    /* Read corner patterns */
    const unsigned char *row = data + (y - 2) * row_stride;
    corners[0] = ((row[x - 2] > 127) << 2) | 
                 ((row[x - 1] > 127) << 1) |
                 (row[x] > 127);

    row = data + (y - 2) * row_stride;
    corners[1] = ((row[x + size - 3] > 127) << 2) |
                 ((row[x + size - 2] > 127) << 1) |
                 (row[x + size - 1] > 127);

    row = data + (y + size - 1) * row_stride;
    corners[2] = ((row[x + size - 3] > 127) << 2) |
                 ((row[x + size - 2] > 127) << 1) |
                 (row[x + size - 1] > 127);

    row = data + (y + size - 1) * row_stride;
    corners[3] = ((row[x - 2] > 127) << 2) |
                 ((row[x - 1] > 127) << 1) |
                 (row[x] > 127);

    /* Match corner patterns */
    if (corners[0] == AZTEC_CORNER_UL && corners[1] == AZTEC_CORNER_UR &&
        corners[2] == AZTEC_CORNER_LR && corners[3] == AZTEC_CORNER_LL) {
        finder->orientation = 0;
    }
    else if (corners[3] == AZTEC_CORNER_UL && corners[0] == AZTEC_CORNER_UR &&
             corners[1] == AZTEC_CORNER_LR && corners[2] == AZTEC_CORNER_LL) {
        finder->orientation = 1;
    }
    else if (corners[2] == AZTEC_CORNER_UL && corners[3] == AZTEC_CORNER_UR &&
             corners[0] == AZTEC_CORNER_LR && corners[1] == AZTEC_CORNER_LL) {
        finder->orientation = 2;
    }
    else if (corners[1] == AZTEC_CORNER_UL && corners[2] == AZTEC_CORNER_UR &&
             corners[3] == AZTEC_CORNER_LR && corners[0] == AZTEC_CORNER_LL) {
        finder->orientation = 3;
    }
    else {
        return 0;
    }

    return 1;
}

/* Read mode message to determine data layers */
static int read_mode_message(const zbar_image_t *img, aztec_finder_t *finder, aztec_mode_t *mode)
{
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    const unsigned char *data = zbar_image_get_data(img);
    int row_stride = get_image_stride(img);
    int x = finder->center_x;
    int y = finder->center_y;
    int orientation = finder->orientation;
    int size = finder->is_compact ? AZTEC_COMPACT_CORE : AZTEC_FULL_CORE;
    
    /* Adjust coordinates based on orientation */
    if (orientation == 1) {
        x = finder->center_y;
        y = width - finder->center_x - 1;
    }
    else if (orientation == 2) {
        x = width - finder->center_x - 1;
        y = height - finder->center_y - 1;
    }
    else if (orientation == 3) {
        x = height - finder->center_y - 1;
        y = finder->center_x;
    }

    /* Read mode message bits */
    int mode_bits = 0;
    int bit_count = 0;
    
    /* Read 5 bits from the mode message area */
    for (int i = 0; i < 5; i++) {
        int bit_x = x + size + i;
        int bit_y = y;
        
        if (bit_x < width && bit_y < height) {
            const unsigned char *row = data + bit_y * row_stride;
            int bit = (row[bit_x] > 127) ? 1 : 0;
            mode_bits = (mode_bits << 1) | bit;
            bit_count++;
        }
    }
    
    if (bit_count < 5) {
        return 0;
    }
    
    /* Decode mode message */
    int layers = (mode_bits >> 1) & 0x0F;
    int data_codewords = 0;
    int ecc_codewords = 0;
    
    if (finder->is_compact) {
        if (layers <= 2) {
            data_codewords = 2 * layers;
            ecc_codewords = 2;
        } else {
            data_codewords = 4 * layers - 4;
            ecc_codewords = 4;
        }
    } else {
        if (layers <= 4) {
            data_codewords = 4 * layers;
            ecc_codewords = 4;
        } else {
            data_codewords = 4 * layers + 8;
            ecc_codewords = 8;
        }
    }
    
    mode->layers = layers;
    mode->data_codewords = data_codewords;
    mode->ecc_codewords = ecc_codewords;
    
    return 1;
}

/* Extract data bits from the symbol */
static int extract_data_bits(const zbar_image_t *img, aztec_finder_t *finder, 
                           aztec_mode_t *mode, unsigned char *bits, int max_bits)
{
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    const unsigned char *data = zbar_image_get_data(img);
    int row_stride = get_image_stride(img);
    int x = finder->center_x;
    int y = finder->center_y;
    int orientation = finder->orientation;
    int size = finder->is_compact ? AZTEC_COMPACT_CORE : AZTEC_FULL_CORE;
    
    /* Adjust coordinates based on orientation */
    if (orientation == 1) {
        x = finder->center_y;
        y = width - finder->center_x - 1;
    }
    else if (orientation == 2) {
        x = width - finder->center_x - 1;
        y = height - finder->center_y - 1;
    }
    else if (orientation == 3) {
        x = height - finder->center_y - 1;
        y = finder->center_x;
    }

    int bit_count = 0;
    int total_codewords = mode->data_codewords + mode->ecc_codewords;
    int bits_per_codeword = finder->is_compact ? 6 : 8;
    int total_bits = total_codewords * bits_per_codeword;
    
    /* Extract bits in spiral pattern */
    int layer = 0;
    int pos = 0;
    int direction = 0; /* 0=right, 1=down, 2=left, 3=up */
    int step = 1;
    int step_count = 0;
    
    while (bit_count < total_bits && bit_count < max_bits) {
        int bit_x = x + size + layer + pos;
        int bit_y = y + layer;
        
        if (bit_x >= 0 && bit_x < width && bit_y >= 0 && bit_y < height) {
            const unsigned char *row = data + bit_y * row_stride;
            int bit = (row[bit_x] > 127) ? 1 : 0;
            bits[bit_count] = bit;
            bit_count++;
        }
        
        /* Move to next position */
        switch (direction) {
            case 0: pos++; break;
            case 1: pos++; break;
            case 2: pos--; break;
            case 3: pos--; break;
        }
        
        step_count++;
        if (step_count >= step) {
            direction = (direction + 1) % 4;
            step_count = 0;
            if (direction % 2 == 0) {
                step++;
            }
        }
        
        /* Check if we need to move to next layer */
        if (pos >= size - 2 * layer) {
            layer++;
            pos = 0;
            direction = 0;
            step = 1;
            step_count = 0;
        }
    }
    
    return bit_count;
}

/* Apply Reed-Solomon error correction */
static int apply_error_correction(unsigned char *data, int data_len, int ecc_len)
{
    /* Simplified error correction - in a real implementation,
       this would use proper Reed-Solomon decoding */
    if (data_len <= 0 || ecc_len <= 0) {
        return 0;
    }
    
    /* For now, just return the data length without error correction */
    return data_len;
}

/* Decode Aztec data from bit stream */
static int decode_aztec_data(unsigned char *bits, int bit_count,
                           unsigned char *output, int max_output)
{
    if (bit_count <= 0 || max_output <= 0) {
        return 0;
    }
    
    /* Simplified decoding - in a real implementation,
       this would properly decode the Aztec data */
    int output_len = 0;
    
    /* Convert bits to bytes (simplified) */
    for (int i = 0; i < bit_count && output_len < max_output - 1; i += 8) {
        unsigned char byte = 0;
        for (int j = 0; j < 8 && i + j < bit_count; j++) {
            byte = (byte << 1) | bits[i + j];
        }
        output[output_len++] = byte;
    }
    
    output[output_len] = '\0';
    return output_len;
}

/* Main Aztec finder detection function */
static inline int aztec_decode_finder(zbar_decoder_t *dcode)
{
    /* This function is called from the 1D scanner context,
       but Aztec is a 2D code that needs image scanning */
    return -1;
}

/* Main Aztec decode function */
zbar_symbol_type_t _zbar_decode_aztec(zbar_decoder_t *dcode)
{
    /* Aztec decoding is handled by the image scanner, not the 1D decoder */
    return ZBAR_NONE;
}

/* 2D image scanning for Aztec codes */
void _zbar_aztec_scan_image(void *iscn_ptr, void *img_ptr)
{
    if (!img_ptr || !iscn_ptr) {
        return;
    }

    zbar_image_scanner_t *iscn = (zbar_image_scanner_t *)iscn_ptr;
    const zbar_image_t *img = (const zbar_image_t *)img_ptr;
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    
    if (width <= 0 || height <= 0 || width > 10000 || height > 10000) {
        return;
    }
    
    int min_size = AZTEC_FULL_FINDER * 2 + 10;
    if (width < min_size || height < min_size) {
        return;
    }
    
    const uint8_t *data = zbar_image_get_data(img);
    if (!data) {
        return;
    }

    int search_step = 8;
    int patterns_checked = 0;
    int max_patterns = (width * height) / (search_step * search_step) + 100;
    
    for (int y = AZTEC_FULL_FINDER; y < height - AZTEC_FULL_FINDER && patterns_checked < max_patterns; y += search_step) {
        for (int x = AZTEC_FULL_FINDER; x < width - AZTEC_FULL_FINDER && patterns_checked < max_patterns; x += search_step) {
            patterns_checked++;
            
            if (x - 6 < 0 || x + 6 >= width || y - 6 < 0 || y + 6 >= height) {
                continue;
            }
            
            int is_compact = 0;
            int finder_size = detect_bullseye_pattern(img, x, y, &is_compact);
            
            if (finder_size > 0) {
                aztec_finder_t finder;
                finder.center_x = x;
                finder.center_y = y;
                finder.size = finder_size;
                finder.is_compact = is_compact;
                finder.orientation = 0;
                
                if (determine_orientation(img, &finder)) {
                    aztec_mode_t mode;
                    if (read_mode_message(img, &finder, &mode)) {
                        unsigned char bits[1024];
                        int bit_count = extract_data_bits(img, &finder, &mode, bits, sizeof(bits));
                        
                        if (bit_count > 0) {
                            unsigned char decoded_data[256];
                            int data_len = decode_aztec_data(bits, bit_count, decoded_data, sizeof(decoded_data));
                            
                            if (data_len > 0) {
                                /* Create symbol and add to results */
                                zbar_symbol_t *sym = _zbar_image_scanner_alloc_sym(iscn, ZBAR_AZTEC, data_len + 1);
                                if (sym) {
                                    memcpy(sym->data, decoded_data, data_len + 1);
                                    sym->datalen = data_len;
                                    sym->type = ZBAR_AZTEC;
                                    sym->quality = 1;
                                    
                                    /* Add position information */
                                    sym->pts = malloc(sizeof(point_t) * 4);
                                    if (sym->pts) {
                                        sym->pts[0].x = x - finder_size/2;
                                        sym->pts[0].y = y - finder_size/2;
                                        sym->pts[1].x = x + finder_size/2;
                                        sym->pts[1].y = y - finder_size/2;
                                        sym->pts[2].x = x + finder_size/2;
                                        sym->pts[2].y = y + finder_size/2;
                                        sym->pts[3].x = x - finder_size/2;
                                        sym->pts[3].y = y + finder_size/2;
                                        sym->npts = 4;
                                    }
                                    
                                    _zbar_image_scanner_add_sym(iscn, sym);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}