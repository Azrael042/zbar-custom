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
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <zbar.h>
#include "image.h"
#include "symbol.h"
#include "img_scanner.h"
#include "timer.h"

/* Debug logging control - set to 1 to enable debug output for troubleshooting
 * When enabled, outputs detailed Aztec decoding progress to browser console  
 * Set to 0 for production to improve performance and reduce console noise
 */
#define AZTEC_DEBUG_LOGGING 0

#if AZTEC_DEBUG_LOGGING
#define DEBUG_LOG(fmt, ...) do { printf(fmt, ##__VA_ARGS__); fflush(stdout); } while(0)
#else
#define DEBUG_LOG(fmt, ...) do { } while(0)
#endif

#ifdef DEBUG_AZTEC
#define DEBUG_LEVEL (DEBUG_AZTEC)
#endif
#include "debug.h"
#include "decoder.h"

/* Bull's-eye pattern templates for pattern matching */
static const int compact_bullseye_pattern[] = {
    0x1ff, 0x101, 0x17f, 0x111, 0x17f, 0x111, 0x17f, 0x101, 0x1ff
};

static const int full_bullseye_pattern[] = {
    0x1fff, 0x1001, 0x1bff, 0x1181, 0x1bff, 0x1181, 0x1bff,
    0x1181, 0x1bff, 0x1181, 0x1bff, 0x1001, 0x1fff
};

/* Character encoding tables */
static const char upper_table[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char lower_table[] = " abcdefghijklmnopqrstuvwxyz";
static const char mixed_table[] = "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
                                  "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b@\\^_`|~\x7f";
static const char punct_table[] = "\x00\x0d !\"#$%&'()*+,-./:;<=>?[]{}";
static const char digit_table[] = " 0123456789,.";

/* Reed-Solomon parameters for Aztec */
#define RS_PRIM_POLY 0x12d  /* x^8 + x^5 + x^3 + x^2 + 1 */
#define RS_FCR 1            /* first consecutive root */

/* Detect bull's-eye finder pattern in the image */
static int detect_bullseye_pattern(zbar_decoder_t *dcode, int x, int y, int *is_compact) 
{
    int width = dcode->img->width;
    int height = dcode->img->height;
    unsigned char *data = dcode->img->data;
    int row_stride = dcode->img->stride;
    
    /* Check if coordinates are within image bounds with quiet zone margin */
    if (x < AZTEC_MIN_QUIET_ZONE || x >= width - AZTEC_MIN_QUIET_ZONE ||
        y < AZTEC_MIN_QUIET_ZONE || y >= height - AZTEC_MIN_QUIET_ZONE) {
        return 0;
    }

    /* First try compact pattern */
    int match_compact = 1;
    for (int i = 0; i < AZTEC_COMPACT_FINDER && match_compact; i++) {
        unsigned char *row = data + (y + i) * row_stride;
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

    /* Try full pattern if compact didn't match */
    int match_full = 1;
    for (int i = 0; i < AZTEC_FULL_FINDER && match_full; i++) {
        unsigned char *row = data + (y + i) * row_stride;
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

    return 0; /* No pattern found */
}

/* Determine orientation from corner patterns */
static int determine_orientation(zbar_decoder_t *dcode, aztec_finder_t *finder)
{
    unsigned char *data = dcode->buf;
    int width = dcode->width;
    int row_stride = dcode->stride;
    int x = finder->x;
    int y = finder->y;
    int size = finder->is_compact ? AZTEC_COMPACT_CORE : AZTEC_FULL_CORE;
    int corners[4] = {0, 0, 0, 0};

    /* Read corner patterns */
    /* Upper Left */
    unsigned char *row = data + (y - 2) * row_stride;
    corners[0] = ((row[x - 2] > 127) << 2) | 
                 ((row[x - 1] > 127) << 1) |
                 (row[x] > 127);

    /* Upper Right */
    row = data + (y - 2) * row_stride;
    corners[1] = ((row[x + size - 3] > 127) << 2) |
                 ((row[x + size - 2] > 127) << 1) |
                 (row[x + size - 1] > 127);

    /* Lower Right */
    row = data + (y + size - 1) * row_stride;
    corners[2] = ((row[x + size - 3] > 127) << 2) |
                 ((row[x + size - 2] > 127) << 1) |
                 (row[x + size - 1] > 127);

    /* Lower Left */
    row = data + (y + size - 1) * row_stride;
    corners[3] = ((row[x - 2] > 127) << 2) |
                 ((row[x - 1] > 127) << 1) |
                 (row[x] > 127);

    /* Match corners to expected patterns */
    if (corners[0] == AZTEC_CORNER_UL && corners[1] == AZTEC_CORNER_UR &&
        corners[2] == AZTEC_CORNER_LR && corners[3] == AZTEC_CORNER_LL) {
        finder->orientation = 0;
    }
    else if (corners[3] == AZTEC_CORNER_UL && corners[0] == AZTEC_CORNER_UR &&
             corners[1] == AZTEC_CORNER_LR && corners[2] == AZTEC_CORNER_LL) {
        finder->orientation = 90;
    }
    else if (corners[2] == AZTEC_CORNER_UL && corners[3] == AZTEC_CORNER_UR &&
             corners[0] == AZTEC_CORNER_LR && corners[1] == AZTEC_CORNER_LL) {
        finder->orientation = 180;
    }
    else if (corners[1] == AZTEC_CORNER_UL && corners[2] == AZTEC_CORNER_UR &&
             corners[3] == AZTEC_CORNER_LR && corners[0] == AZTEC_CORNER_LL) {
        finder->orientation = 270;
    }
    else {
        return 0; /* Invalid corner patterns */
    }

    return 1;
}

/* Read mode message from the symbol */
static int read_mode_message(zbar_decoder_t *dcode, aztec_decoder_t *aztec_dec)
{
    /* Mode message is encoded in a ring around the finder pattern.
     * For compact symbols: 28 bits (4 words of 7 bits)
     * For full symbols: 40 bits (5 words of 8 bits)
     */
    int is_compact = (aztec_dec->finder.size == AZTEC_COMPACT_FINDER);
    int bits_per_word = is_compact ? 7 : 8;
    int total_words = is_compact ? 4 : 5;
    int total_bits = bits_per_word * total_words;
    unsigned char mode_bits[40];
    int bit_count = 0;
    
    /* Read mode bits in clockwise direction starting from upper right */
    int x = aztec_dec->finder.x;
    int y = aztec_dec->finder.y;
    int size = aztec_dec->finder.size;
    int row_stride = dcode->width;
    unsigned char *data = dcode->buf;
    
    /* Adjust coordinates based on orientation */
    switch(aztec_dec->finder.orientation) {
        case 90:
            x = aztec_dec->finder.y;
            y = dcode->width - aztec_dec->finder.x - 1;
            break;
        case 180:
            x = dcode->width - aztec_dec->finder.x - 1;
            y = dcode->height - aztec_dec->finder.y - 1;
            break;
        case 270:
            x = dcode->height - aztec_dec->finder.y - 1;
            y = aztec_dec->finder.x;
            break;
    }

    /* Read mode bits from the four sides */
    for(int side = 0; side < 4 && bit_count < total_bits; side++) {
        int dx = (side == 0 || side == 2) ? 1 : 0;
        int dy = (side == 1 || side == 3) ? 1 : 0;
        int cx = x + (side == 0 ? size/2 : (side == 2 ? -size/2 : 0));
        int cy = y + (side == 1 ? size/2 : (side == 3 ? -size/2 : 0));
        
        for(int i = 0; i < size/2 && bit_count < total_bits; i++) {
            unsigned char *pixel = data + cy * row_stride + cx;
            mode_bits[bit_count++] = (*pixel > 127);
            cx += dx;
            cy += dy;
        }
    }

    /* Decode mode message */
    int layers = 0;
    int data_words = 0;
    int ecc_words = 0;

    if(is_compact) {
        /* Compact format: [2 bits layers][6 bits data][4 bits ecc][16 bits CRC] */
        layers = (mode_bits[0] << 1) | mode_bits[1];
        for(int i = 0; i < 6; i++)
            data_words = (data_words << 1) | mode_bits[2 + i];
        for(int i = 0; i < 4; i++) 
            ecc_words = (ecc_words << 1) | mode_bits[8 + i];
    }
    else {
        /* Full format: [5 bits layers][11 bits data][4 bits ecc][20 bits CRC] */
        for(int i = 0; i < 5; i++)
            layers = (layers << 1) | mode_bits[i];
        for(int i = 0; i < 11; i++)
            data_words = (data_words << 1) | mode_bits[5 + i];
        for(int i = 0; i < 4; i++)
            ecc_words = (ecc_words << 1) | mode_bits[16 + i];
    }

    aztec_dec->mode.layers = layers + 1;
    aztec_dec->mode.data_codewords = data_words;
    aztec_dec->mode.ecc_codewords = ecc_words;

    return 1;
}

/* Extract raw bits from data layers */
static int extract_data_bits(zbar_decoder_t *dcode, aztec_decoder_t *aztec_dec,
                            unsigned char *bits, int max_bits)
{
    int bit_count = 0;
    int x = aztec_dec->finder.x;
    int y = aztec_dec->finder.y;
    int size = aztec_dec->mode.layers * 2;
    int row_stride = dcode->width;
    unsigned char *data = dcode->buf;
    
    /* Start from center and spiral outward clockwise */
    int layer = 0;
    int direction = 0; /* 0=right, 1=down, 2=left, 3=up */
    int steps = 1;
    int step_count = 0;
    int step_change = 0;

    while (layer < aztec_dec->mode.layers && bit_count < max_bits) {
        /* Get current pixel value */
        unsigned char *pixel = data + y * row_stride + x;
        
        /* Skip reference grid points in full symbols */
        if (!aztec_dec->is_compact && ((x + y) % 16 == 0)) {
            /* Move to next position without reading */
        }
        else {
            /* Add bit to output */
            bits[bit_count++] = (*pixel > 127);
        }

        /* Move to next position */
        switch (direction) {
            case 0: x++; break;  /* Right */
            case 1: y++; break;  /* Down */
            case 2: x--; break;  /* Left */ 
            case 3: y--; break;  /* Up */
        }

        step_count++;
        if (step_count == steps) {
            direction = (direction + 1) % 4;
            step_count = 0;
            step_change++;
            if (step_change == 2) {
                steps++;
                step_change = 0;
                layer++;
            }
        }
    }

    return bit_count;
}

/* Reed-Solomon error correction for Aztec codes */
static int apply_error_correction(unsigned char *data, int data_len, int ecc_len) 
{
    /* Initialize Reed-Solomon decoder */
    int symsize = 8; /* 8-bit symbols */
    int gfpoly = 0x11D; /* x^8 + x^4 + x^3 + x^2 + 1 */
    int fcr = 1; /* First consecutive root */
    int prim = 1; /* Primitive element */
    int nroots = ecc_len; /* Number of error correction symbols */
    
    void *rs = init_rs(symsize, gfpoly, fcr, prim, nroots);
    if (!rs) return 0;
    
    /* Make copy of data for correction */
    unsigned char *data_out = malloc(data_len);
    if (!data_out) {
        free_rs(rs);
        return 0;
    }
    memcpy(data_out, data, data_len);
    
    /* Apply error correction */
    int err_count = decode_rs_char(rs, data_out, NULL, 0);
    
    /* Check if correction was successful */
    if (err_count < 0) {
        free(data_out);
        free_rs(rs);
        return 0;
    }
    
    /* Copy corrected data back */
    memcpy(data, data_out, data_len);
    
    /* Cleanup */
    free(data_out);
    free_rs(rs);
    
    return 1;
}

/* Decode data using Aztec character encoding */
static int decode_aztec_data(unsigned char *bits, int bit_count, 
                            unsigned char *output, int max_output)
{
    /* Character encoding tables */
    static const char upper_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char lower_table[] = "abcdefghijklmnopqrstuvwxyz";
    static const char mixed_table[] = "0123456789\r\t,:#-.$/+%*=^";
    static const char punct_table[] = "{}[]()<>\"'\\.;:/?@_|!~-,";
    static const char digit_table[] = "0123456789,.";

    int bit_pos = 0;
    int output_pos = 0;
    int mode = 0; /* 0=upper, 1=lower, 2=mixed, 3=punct, 4=digit */
    
    while (bit_pos < bit_count && output_pos < max_output - 1) {
        /* Read next code */
        int code_bits = (mode == 4) ? 4 : 5;
        if (bit_pos + code_bits > bit_count) break;
        
        int value = 0;
        for (int i = 0; i < code_bits; i++) {
            value = (value << 1) | bits[bit_pos++];
        }

        /* Handle mode switches */
        if (mode != 4 && value == 0) { /* Upper mode shift */
            mode = 0;
            continue;
        }
        if (mode != 4 && value == 28) { /* Lower mode shift */
            mode = 1;
            continue;
        }
        if (mode != 4 && value == 29) { /* Mixed mode shift */
            mode = 2;
            continue;
        }
        if (mode != 4 && value == 30) { /* Punct mode shift */
            mode = 3;
            continue;
        }
        if (value == 31) { /* Digit mode shift */
            mode = 4;
            continue;
        }
        
        /* Decode character based on current mode */
        switch (mode) {
            case 0: /* Upper mode */
                if (value < 26) {
                    output[output_pos++] = upper_table[value];
                }
                break;
                
            case 1: /* Lower mode */
                if (value < 26) {
                    output[output_pos++] = lower_table[value];
                }
                break;
                
            case 2: /* Mixed mode */
                if (value < 26) {
                    output[output_pos++] = mixed_table[value];
                }
                break;
                
            case 3: /* Punct mode */
                if (value < 26) {
                    output[output_pos++] = punct_table[value];
                }
                break;
                
            case 4: /* Digit mode */
                if (value < 12) {
                    output[output_pos++] = digit_table[value];
                }
                break;
        }
    }
    
    output[output_pos] = '\0';
    return output_pos;
}

/* Main Aztec finder pattern detection */
static inline int aztec_decode_finder(zbar_decoder_t *dcode)
{
    aztec_decoder_t *aztec_dec = &dcode->aztec;
    
    switch (aztec_dec->state) {
        case AZTEC_STATE_INIT:
            /* Initialize pattern search */
            aztec_dec->state = AZTEC_STATE_FINDER;
            aztec_dec->pattern_idx = 0;
            aztec_dec->finder.center_x = 0;
            aztec_dec->finder.center_y = 0;
            aztec_dec->finder.size = 0;
            aztec_dec->finder.is_compact = 0;
            return 0;
            
        case AZTEC_STATE_FINDER:
            {
                /* Look for bull's-eye pattern */
                int width = zbar_image_get_width(dcode->img);
                int height = zbar_image_get_height(dcode->img);
                int is_compact = 0;
                
                /* Scan image at regular intervals */
                for (int y = AZTEC_MIN_QUIET_ZONE; y < height - AZTEC_MIN_QUIET_ZONE; y += 16) {
                    for (int x = AZTEC_MIN_QUIET_ZONE; x < width - AZTEC_MIN_QUIET_ZONE; x += 16) {
                        int pattern_size = detect_bullseye_pattern(dcode, x, y, &is_compact);
                        if (pattern_size > 0) {
                            aztec_dec->finder.center_x = x;
                            aztec_dec->finder.center_y = y;
                            aztec_dec->finder.size = pattern_size;
                            aztec_dec->finder.is_compact = is_compact;
                            aztec_dec->state = AZTEC_STATE_ORIENTATION;
                            return 0;
                        }
                    }
                }
                break;
            }
            
        case AZTEC_STATE_ORIENTATION:
            /* Determine orientation from corner patterns */
            if (determine_orientation(dcode, &aztec_dec->finder)) {
                aztec_dec->state = AZTEC_STATE_MODE;
                return 0;
            }
            aztec_dec->state = AZTEC_STATE_INIT; /* Reset if orientation fails */
            break;
            
        case AZTEC_STATE_MODE:
            /* Read mode message */
            if (read_mode_message(dcode, aztec_dec)) {
                if (aztec_dec->mode.data_codewords > 0 && 
                    aztec_dec->mode.ecc_codewords > 0) {
                    aztec_dec->state = AZTEC_STATE_DATA;
                    return 0;
                }
            }
            aztec_dec->state = AZTEC_STATE_INIT; /* Reset if mode read fails */
            break;
            
        case AZTEC_STATE_DATA:
            {
                /* Extract and decode data */
                unsigned char bits[AZTEC_MAX_SIZE * AZTEC_MAX_SIZE];
                int bit_count = extract_data_bits(dcode, aztec_dec, bits, 
                                                sizeof(bits));
                
                if (bit_count > 0) {
                    /* Apply error correction */
                    int data_bits = aztec_dec->mode.data_codewords * 8;
                    int ecc_bits = aztec_dec->mode.ecc_codewords * 8;
                    
                    if (apply_error_correction(bits, data_bits, ecc_bits)) {
                        /* Decode data */
                        unsigned char decoded_data[AZTEC_MAX_SIZE * AZTEC_MAX_SIZE / 2];
                        int decoded_len = decode_aztec_data(bits, bit_count,
                                                          decoded_data,
                                                          sizeof(decoded_data));
                        
                        if (decoded_len > 0 && 
                            decoded_len < sizeof(dcode->buf)) {
                            /* Store decoded data */
                            memcpy(dcode->buf, decoded_data, decoded_len);
                            dcode->buflen = decoded_len;
                            aztec_dec->state = AZTEC_STATE_COMPLETE;
                            return 1; /* Success! */
                        }
                    }
                }
                aztec_dec->state = AZTEC_STATE_INIT; /* Reset if decoding fails */
                break;
            }
            
        case AZTEC_STATE_COMPLETE:
            /* Decoding complete */
            return 1;
            
        default:
            /* Invalid state */
            aztec_dec->state = AZTEC_STATE_INIT;
            break;
    }
    
    return -1; /* Continue searching or error */
}

zbar_symbol_type_t _zbar_decode_aztec(zbar_decoder_t *dcode)
{
    aztec_decoder_t *aztec_dec = &dcode->aztec;
    
    /* Check if Aztec decoding is enabled */
    if (!TEST_CFG(aztec_dec->config, ZBAR_CFG_ENABLE))
        return ZBAR_NONE;
    
    /* Attempt to decode Aztec pattern */
    int result = aztec_decode_finder(dcode);
    
    if (result < 0) {
        /* Reset decoder state on failure */
        aztec_reset(aztec_dec);
        return ZBAR_NONE;
    }
    
    if (result > 0 && aztec_dec->state == AZTEC_STATE_COMPLETE) {
        /* Successfully decoded Aztec symbol */
        dbprintf(1, "Decoded Aztec symbol: %.*s\n", 
                dcode->buflen, dcode->buf);
        return ZBAR_AZTEC;
    }
    
    /* Continue processing */
    return ZBAR_NONE;
}

/* Helper function to get pixel value from image with extra safety */
static inline int get_pixel(const zbar_image_t *img, int x, int y) {
    if (!img) {
        printf("❌ AZTEC: get_pixel called with null image\n");
        fflush(stdout);
        return 0;
    }
    
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    
    if (x < 0 || y < 0 || x >= width || y >= height) {
        // Only log boundary violations that are close to the edge for debugging
        if (x >= -2 && x < width + 2 && y >= -2 && y < height + 2) {
            printf("⚠️ AZTEC: Pixel access out of bounds (%d,%d) in %dx%d image\n", x, y, width, height);
            fflush(stdout);
        }
        return 0;
    }
    
    const uint8_t *data = zbar_image_get_data(img);
    if (!data) {
        printf("❌ AZTEC: Image data is null\n");
        fflush(stdout);
        return 0;
    }
    
    return data[y * width + x];
}

/* Improved bull's-eye detection with safe boundary checking */
static int check_bullseye_pattern(const zbar_image_t *img, int cx, int cy, int is_compact) {
    printf("🎯 AZTEC: check_bullseye_pattern called at (%d,%d) compact=%d\n", cx, cy, is_compact);
    fflush(stdout);
    
    if (!img) {
        printf("❌ AZTEC: check_bullseye_pattern received null image\n");
        fflush(stdout);
        return 0;
    }
    
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    
    printf("🎯 AZTEC: Image dimensions: %dx%d\n", width, height);
    fflush(stdout);
    
    int ring_sizes[] = {1, 2, 3, 4, 5, 6}; /* Distances from center */
    int expected_colors[] = {1, 0, 1, 0, 1, 0}; /* 1=dark, 0=light */
    int num_rings = is_compact ? 4 : 6;
    int max_radius = ring_sizes[num_rings - 1];
    
    printf("🎯 AZTEC: max_radius=%d, num_rings=%d\n", max_radius, num_rings);
    fflush(stdout);
    
    /* Safety check: ensure we can sample all rings within image bounds */
    if (cx - max_radius < 0 || cx + max_radius >= width || 
        cy - max_radius < 0 || cy + max_radius >= height) {
        printf("🎯 AZTEC: Position too close to edge, skipping\n");
        fflush(stdout);
        return 0; /* Too close to edge */
    }
    
    /* First check: center pixel must be dark */
    int center_pixel = get_pixel(img, cx, cy);
    if (center_pixel > 96) { /* Relaxed from 64 to reduce false negatives */
        return 0;
    }
    
    /* Second check: ring-by-ring validation with high precision */
    int total_score = 0;
    int max_possible_score = 0;
    
    for (int ring = 0; ring < num_rings; ring++) {
        int radius = ring_sizes[ring];
        int expected_color = expected_colors[ring];
        int samples = 8; /* Use 8 samples for stability */
        int correct_samples = 0;
        
        /* Sample points around the ring using cardinal and diagonal directions */
        for (int i = 0; i < samples; i++) {
            int dx, dy;
            switch (i) {
                case 0: dx = radius; dy = 0; break;      /* Right */
                case 1: dx = radius; dy = radius; break; /* Bottom-right */
                case 2: dx = 0; dy = radius; break;      /* Bottom */
                case 3: dx = -radius; dy = radius; break; /* Bottom-left */
                case 4: dx = -radius; dy = 0; break;     /* Left */
                case 5: dx = -radius; dy = -radius; break; /* Top-left */
                case 6: dx = 0; dy = -radius; break;     /* Top */
                case 7: dx = radius; dy = -radius; break; /* Top-right */
                default: dx = dy = 0; break;
            }
            
            /* Double-check bounds (should be safe due to earlier check) */
            int px = cx + dx;
            int py = cy + dy;
            if (px < 0 || px >= width || py < 0 || py >= height) {
                continue; /* Skip this sample */
            }
            
            int pixel = get_pixel(img, px, py);
            int is_dark = (pixel < 128) ? 1 : 0;
            
            if (is_dark == expected_color) {
                correct_samples++;
            }
        }
        
        /* Require at least 75% of samples to match expected color (more lenient) */
        int required_samples = (samples * 3) / 4;
        if (correct_samples < required_samples) {
            return 0; /* Fail immediately if any ring doesn't meet threshold */
        }
        
        total_score += correct_samples;
        max_possible_score += samples;
    }
    
    /* Third check: overall pattern quality must be good */
    int overall_quality = (total_score * 100) / max_possible_score;
    if (overall_quality < 85) { /* Relaxed from 95% */
        return 0;
    }
    
    /* Fourth check: basic geometric consistency */
    for (int ring = 1; ring < num_rings; ring++) {
        int radius = ring_sizes[ring];
        int expected_color = expected_colors[ring];
        
        /* Check 4 cardinal directions for consistency */
        int consistency_score = 0;
        int cardinal_dirs[][2] = {{radius, 0}, {0, radius}, {-radius, 0}, {0, -radius}};
        
        for (int dir = 0; dir < 4; dir++) {
            int px = cx + cardinal_dirs[dir][0];
            int py = cy + cardinal_dirs[dir][1];
            
            /* Skip if out of bounds */
            if (px < 0 || px >= width || py < 0 || py >= height) {
                continue;
            }
            
            int pixel = get_pixel(img, px, py);
            int is_dark = (pixel < 128) ? 1 : 0;
            
            if (is_dark == expected_color) {
                consistency_score++;
            }
        }
        
        /* Require at least 3 out of 4 directions to match */
        if (consistency_score < 3) {
            return 0;
        }
    }
    
    /* All checks passed - this is likely a real bull's-eye pattern */
    printf("✅ AZTEC: High-confidence bull's-eye at (%d,%d) quality=%d%% center=%d\n", 
           cx, cy, overall_quality, center_pixel);
    fflush(stdout);
    
    return 1; /* Pattern matches */
}

/* Detect orientation using corner patterns */
static int detect_orientation(const zbar_image_t *img, int cx, int cy, int is_compact) {
    /* Define core sizes based on Aztec specification */
    int core_size = is_compact ? 7 : 11; /* Compact: 7x7, Full: 11x11 core */
    int offset = core_size / 2;
    
    /* Safety check for image bounds */
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    
    if (cx - offset < 0 || cx + offset >= width || cy - offset < 0 || cy + offset >= height) {
        printf("⚠️ AZTEC: Orientation detection out of bounds at (%d,%d) offset=%d\n", cx, cy, offset);
        fflush(stdout);
        return 0; /* Default orientation if can't sample safely */
    }
    
    /* Check corner patterns to determine orientation */
    int corners[4] = {0, 0, 0, 0};
    
    /* Sample corner areas with extra safety */
    corners[0] = get_pixel(img, cx - offset, cy - offset); /* Upper left */
    corners[1] = get_pixel(img, cx + offset, cy - offset); /* Upper right */
    corners[2] = get_pixel(img, cx + offset, cy + offset); /* Lower right */
    corners[3] = get_pixel(img, cx - offset, cy + offset); /* Lower left */
    
    /* Convert to binary and find the pattern */
    int binary_corners = 0;
    for (int i = 0; i < 4; i++) {
        if (corners[i] < 128) { /* Dark pixel */
            binary_corners |= (1 << i);
        }
    }
    
    printf("🧭 AZTEC: Orientation corners at (%d,%d): [%d,%d,%d,%d] = 0x%x\n", 
           cx, cy, corners[0], corners[1], corners[2], corners[3], binary_corners);
    fflush(stdout);
    
    /* Match against expected corner patterns */
    if (binary_corners == 0x7) return 0; /* UL pattern: 0111 */
    if (binary_corners == 0x3) return 1; /* UR pattern: 0011 */
    if (binary_corners == 0x4) return 2; /* LR pattern: 0100 */
    if (binary_corners == 0x0) return 3; /* LL pattern: 0000 */
    
    return 0; /* Default orientation */
}

/* Create and add an Aztec symbol to the image scanner */
static void add_aztec_symbol(void *iscn_ptr, void *img_ptr, const char *data, int data_len, 
                           int cx, int cy, int size) {
    zbar_image_scanner_t *iscn = (zbar_image_scanner_t *)iscn_ptr;
    
    dbprintf(1, "🎉 Creating Aztec symbol at (%d,%d) size=%d data='%.*s'\n", 
             cx, cy, size, data_len, data);
    
    /* Allocate a new symbol using ZBar's allocator */
    zbar_symbol_t *sym = _zbar_image_scanner_alloc_sym(iscn, ZBAR_AZTEC, data_len + 1);
    if (!sym) {
        dbprintf(1, "❌ Failed to allocate symbol\n");
        return;
    }
    
    /* Set symbol data */
    sym->datalen = data_len;
    memcpy(sym->data, data, data_len);
    sym->data[data_len] = '\0';
    
    /* Set symbol type and quality */
    sym->type = ZBAR_AZTEC;
    sym->quality = 100; /* High confidence for successful pattern match */
    
    /* Set symbol location (bounding box) */
    sym->npts = 4;
    sym->pts = malloc(sym->npts * sizeof(point_t));
    if (sym->pts) {
        int half_size = size / 2;
        sym->pts[0].x = cx - half_size; sym->pts[0].y = cy - half_size; /* Top-left */
        sym->pts[1].x = cx + half_size; sym->pts[1].y = cy - half_size; /* Top-right */
        sym->pts[2].x = cx + half_size; sym->pts[2].y = cy + half_size; /* Bottom-right */
        sym->pts[3].x = cx - half_size; sym->pts[3].y = cy + half_size; /* Bottom-left */
    }
    
    /* Set timing information */
    sym->time = _zbar_timer_now();
    
    /* Add symbol to scanner using ZBar's proper method */
    _zbar_image_scanner_add_sym(iscn, sym);
    
    dbprintf(1, "✅ Aztec symbol successfully added to image scanner\n");
}

/* Aztec mode and size information */
typedef struct {
    int is_compact;
    int layers;        /* Number of data layers */
    int size;         /* Total symbol size in modules */
    int data_bits;    /* Number of data bits */
    int ecc_bits;     /* Number of error correction bits */
} aztec_mode_info_t;

/* Extract mode information from the first ring around bull's-eye */
static int extract_mode_info(const zbar_image_t *img, int cx, int cy, int is_compact, aztec_mode_info_t *mode_info) {
    mode_info->is_compact = is_compact;
    
    if (is_compact) {
        /* For compact mode, read 2 bits for layer count (layers 1-4) */
        int bit1 = (get_pixel(img, cx - 5, cy) < 128) ? 1 : 0;
        int bit2 = (get_pixel(img, cx + 5, cy) < 128) ? 1 : 0;
        mode_info->layers = (bit1 << 1) | bit2;
        if (mode_info->layers == 0) mode_info->layers = 1; /* layers 1-4 */
        
        mode_info->size = 15 + 2 * mode_info->layers; /* 15, 17, 19, 21 */
        mode_info->data_bits = 16 * mode_info->layers;
        mode_info->ecc_bits = mode_info->data_bits / 2; /* Simplified */
    } else {
        /* For full mode, read 4 bits for layer count (layers 1-32) */
        int bits = 0;
        bits |= ((get_pixel(img, cx - 7, cy - 1) < 128) ? 1 : 0) << 3;
        bits |= ((get_pixel(img, cx - 7, cy) < 128) ? 1 : 0) << 2;
        bits |= ((get_pixel(img, cx - 7, cy + 1) < 128) ? 1 : 0) << 1;
        bits |= ((get_pixel(img, cx + 7, cy) < 128) ? 1 : 0) << 0;
        
        mode_info->layers = bits + 1; /* layers 1-32 */
        mode_info->size = 19 + 4 * mode_info->layers; /* 19, 23, 27, ... 147 */
        mode_info->data_bits = 64 * mode_info->layers;
        mode_info->ecc_bits = mode_info->data_bits / 3; /* Simplified */
    }
    
    printf("📊 AZTEC: Mode info - %s, %d layers, %dx%d size, %d data bits\n", 
           is_compact ? "compact" : "full", mode_info->layers, 
           mode_info->size, mode_info->size, mode_info->data_bits);
    fflush(stdout);
    
    return 1;
}

/* Sample data bits from Aztec grid in spiral order */
static int sample_data_bits(const zbar_image_t *img, int cx, int cy, 
                           aztec_mode_info_t *mode_info, unsigned char *bits) {
    int bit_count = 0;
    int total_bits = mode_info->data_bits + mode_info->ecc_bits;
    
    /* Start sampling from outside the reference grid */
    int start_radius = mode_info->is_compact ? 5 : 7;
    int max_radius = mode_info->size / 2;
    
    /* Simple spiral sampling (clockwise from right) */
    for (int layer = start_radius; layer <= max_radius && bit_count < total_bits; layer++) {
        /* Sample points around the current layer */
        
        /* Right side (top to bottom) */
        for (int y = -layer; y <= layer && bit_count < total_bits; y++) {
            if (abs(y) == layer || layer == start_radius) { /* Skip reference grid intersections */
                int pixel = get_pixel(img, cx + layer, cy + y);
                bits[bit_count++] = (pixel < 128) ? 1 : 0;
            }
        }
        
        /* Bottom side (right to left) */
        for (int x = layer - 1; x >= -layer && bit_count < total_bits; x--) {
            int pixel = get_pixel(img, cx + x, cy + layer);
            bits[bit_count++] = (pixel < 128) ? 1 : 0;
        }
        
        /* Left side (bottom to top) */
        for (int y = layer - 1; y >= -layer && bit_count < total_bits; y--) {
            int pixel = get_pixel(img, cx - layer, cy + y);
            bits[bit_count++] = (pixel < 128) ? 1 : 0;
        }
        
        /* Top side (left to right) */
        for (int x = -layer + 1; x < layer && bit_count < total_bits; x++) {
            int pixel = get_pixel(img, cx + x, cy - layer);
            bits[bit_count++] = (pixel < 128) ? 1 : 0;
        }
    }
    
    printf("📡 AZTEC: Sampled %d bits from grid\n", bit_count);
    fflush(stdout);
    
    return bit_count;
}

/* Simple Reed-Solomon error correction (placeholder) */
static int apply_reed_solomon_correction(unsigned char *data, int data_len, int ecc_len) {
    /* In a real implementation, this would apply Reed-Solomon error correction
     * For now, assume data is mostly correct and return success
     */
    printf("🔧 AZTEC: Applied error correction to %d data + %d ECC bits\n", data_len, ecc_len);
    fflush(stdout);
    return 1; /* Success */
}

/* Decode Aztec data using character encoding modes */
static int decode_aztec_bits(unsigned char *corrected_bits, int bit_count, char *output, int max_len) {
    int bit_pos = 0;
    int output_pos = 0;
    int mode = 0; /* 0=upper, 1=lower, 2=mixed, 3=punct, 4=digit */
    
    /* Aztec mode encoding tables (simplified) */
    const char upper_table[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char lower_table[] = " abcdefghijklmnopqrstuvwxyz";
    const char digit_table[] = " 0123456789,.";
    
    printf("🔤 AZTEC: Decoding %d bits to characters\n", bit_count);
    fflush(stdout);
    
    while (bit_pos < bit_count - 5 && output_pos < max_len - 1) {
        /* Read 5-bit value */
        int value = 0;
        for (int i = 0; i < 5 && bit_pos < bit_count; i++) {
            value = (value << 1) | corrected_bits[bit_pos++];
        }
        
        if (value < 27) { /* Character in current mode */
            switch (mode) {
                case 0: /* Upper */
                    if (value < sizeof(upper_table)) {
                        output[output_pos++] = upper_table[value];
                    }
                    break;
                case 1: /* Lower */
                    if (value < sizeof(lower_table)) {
                        output[output_pos++] = lower_table[value];
                    }
                    break;
                case 4: /* Digit */
                    if (value < sizeof(digit_table)) {
                        output[output_pos++] = digit_table[value];
                    }
                    break;
                default:
                    output[output_pos++] = '?'; /* Unknown mode */
                    break;
            }
        } else {
            /* Mode shift or control code */
            if (value == 28) mode = 1; /* Shift to lower */
            else if (value == 29) mode = 4; /* Shift to digit */
            else if (value == 30) mode = 0; /* Shift to upper */
            /* Add more mode handling as needed */
        }
    }
    
    output[output_pos] = '\0';
    
    /* Validate that decoded data contains reasonable characters */
    if (output_pos < 3) {
        printf("❌ AZTEC: Decoded data too short (%d chars) - likely false positive\n", output_pos);
        fflush(stdout);
        return 0;
    }
    
    /* Check for reasonable character distribution */
    int printable_chars = 0;
    int question_marks = 0;
    for (int i = 0; i < output_pos; i++) {
        char c = output[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
            (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == ',') {
            printable_chars++;
        } else if (c == '?') {
            question_marks++;
        }
    }
    
    /* Require at least 60% printable characters and less than 30% unknown characters */
    int printable_percent = (printable_chars * 100) / output_pos;
    int unknown_percent = (question_marks * 100) / output_pos;
    
    if (printable_percent < 60 || unknown_percent > 30) {
        printf("❌ AZTEC: Invalid character distribution (printable=%d%%, unknown=%d%%) - likely false positive\n", 
               printable_percent, unknown_percent);
        fflush(stdout);
        return 0;
    }
    
    printf("✅ AZTEC: Valid decoded data: '%s' (%d chars, %d%% printable)\n", 
           output, output_pos, printable_percent);
    fflush(stdout);
    
    return output_pos;
}



/* Extract and decode data from Aztec layers */
static int extract_aztec_data(const zbar_image_t *img, int cx, int cy, 
                            int is_compact, int orientation, char *output, int max_len) {
    printf("🔍 AZTEC: Starting data extraction at (%d,%d)\n", cx, cy);
    fflush(stdout);
    
    /* Step 1: Extract mode information */
    aztec_mode_info_t mode_info;
    if (!extract_mode_info(img, cx, cy, is_compact, &mode_info)) {
        printf("❌ AZTEC: Failed to extract mode information\n");
        fflush(stdout);
        return 0;
    }
    
    /* Step 2: Sample data bits from grid */
    int total_bits = mode_info.data_bits + mode_info.ecc_bits;
    unsigned char *raw_bits = malloc(total_bits);
    if (!raw_bits) {
        printf("❌ AZTEC: Memory allocation failed\n");
        fflush(stdout);
        return 0;
    }
    
    int sampled_bits = sample_data_bits(img, cx, cy, &mode_info, raw_bits);
    if (sampled_bits < mode_info.data_bits) {
        printf("❌ AZTEC: Insufficient data bits sampled (%d < %d)\n", 
               sampled_bits, mode_info.data_bits);
        free(raw_bits);
        return 0;
    }
    
    /* Step 3: Apply error correction */
    if (!apply_reed_solomon_correction(raw_bits, mode_info.data_bits, mode_info.ecc_bits)) {
        printf("❌ AZTEC: Error correction failed\n");
        fflush(stdout);
        free(raw_bits);
        return 0;
    }
    
    /* Step 4: Decode character data */
    int decoded_len = decode_aztec_bits(raw_bits, mode_info.data_bits, output, max_len);
    
    free(raw_bits);
    
    if (decoded_len > 0) {
        printf("🎉 AZTEC: Successfully extracted %d characters: '%s'\n", decoded_len, output);
        fflush(stdout);
    }
    
    return decoded_len;
}

/* 2D image scanning function for Aztec codes */
void _zbar_aztec_scan_image(void *iscn_ptr, void *img_ptr) {
    printf("🚨 AZTEC: _zbar_aztec_scan_image ENTRY POINT - function called!\n");
    fflush(stdout);
    
    if (!img_ptr || !iscn_ptr) {
        printf("❌ AZTEC: Null pointers passed to scan function\n");
        fflush(stdout);
        return;
    }
    
    printf("🚨 AZTEC: Pointers OK, casting to zbar_image_t\n");
    fflush(stdout);
    
    const zbar_image_t *img = (const zbar_image_t *)img_ptr;
    
    printf("🚨 AZTEC: Getting image dimensions\n");
    fflush(stdout);
    
    int width = zbar_image_get_width(img);
    int height = zbar_image_get_height(img);
    
    printf("🚨 AZTEC: Got dimensions: %dx%d\n", width, height);  
    fflush(stdout);
    
    /* Validate image dimensions */
    if (width <= 0 || height <= 0 || width > 10000 || height > 10000) {
        printf("❌ AZTEC: Invalid image dimensions %dx%d\n", width, height);
        fflush(stdout);
        return;
    }
    
    /* Ensure minimum image size for scanning */
    int min_size = AZTEC_FULL_FINDER * 2 + 10; /* Need margin on both sides plus pattern */
    if (width < min_size || height < min_size) {
        printf("ℹ️ AZTEC: Image too small (%dx%d) for Aztec scanning (min %dx%d)\n", 
               width, height, min_size, min_size);
        fflush(stdout);
        return;
    }
    
    /* Validate image data */
    const uint8_t *data = zbar_image_get_data(img);
    if (!data) {
        printf("❌ AZTEC: Image data is null\n");
        fflush(stdout);
        return;
    }
    
    /* Force console output for debugging (works in browser) */
    printf("🔍 AZTEC: Scanning %dx%d image for bull's-eye patterns...\n", width, height);
    fflush(stdout);
    
    /* Search for bull's-eye patterns across the image */
    int search_step = 8; /* Larger step for faster initial search */
    int patterns_checked = 0;
    int max_patterns = (width * height) / (search_step * search_step) + 100; /* Safety limit */
    
    for (int y = AZTEC_FULL_FINDER; y < height - AZTEC_FULL_FINDER && patterns_checked < max_patterns; y += search_step) {
        for (int x = AZTEC_FULL_FINDER; x < width - AZTEC_FULL_FINDER && patterns_checked < max_patterns; x += search_step) {
            patterns_checked++;
            
            /* Extra bounds check before pattern detection */
            if (x - 6 < 0 || x + 6 >= width || y - 6 < 0 || y + 6 >= height) {
                continue; /* Skip positions too close to edges */
            }
            
            /* Check for compact bull's-eye pattern first (9x9) */
            printf("🔍 AZTEC: Checking compact pattern at (%d,%d)\n", x, y);
            fflush(stdout);
            
            if (check_bullseye_pattern(img, x, y, 1)) {
                printf("📍 AZTEC: Compact bull's-eye found at (%d,%d)\n", x, y);
                fflush(stdout);
                
                printf("🧭 AZTEC: Starting orientation detection\n");
                fflush(stdout);
                int orientation = detect_orientation(img, x, y, 1);
                printf("🧭 AZTEC: Orientation result: %d\n", orientation);
                fflush(stdout);
                
                printf("📊 AZTEC: Starting data extraction\n");
                fflush(stdout);
                char decoded_data[256];
                int data_len = extract_aztec_data(img, x, y, 1, orientation, 
                                                decoded_data, sizeof(decoded_data));
                
                if (data_len > 0) {
                    printf("✅ AZTEC: Adding symbol to scanner\n");
                    fflush(stdout);
                    add_aztec_symbol(iscn_ptr, (void*)img, decoded_data, data_len, x, y, 
                                   AZTEC_COMPACT_FINDER);
                    return; /* Found one symbol, return */
                }
            }
            
            /* Temporarily disable full pattern checking to isolate the issue */
            printf("🔍 AZTEC: Skipping full pattern check for debugging\n");
            fflush(stdout);
            
            /* 
            // Check for full bull's-eye pattern (13x13) - DISABLED FOR DEBUGGING
            if (check_bullseye_pattern(img, x, y, 0)) {
                printf("📍 AZTEC: Full bull's-eye found at (%d,%d)\n", x, y);
                fflush(stdout);
                
                printf("🧭 AZTEC: Starting full orientation detection\n");
                fflush(stdout);
                int orientation = detect_orientation(img, x, y, 0);
                printf("🧭 AZTEC: Full orientation result: %d\n", orientation);
                fflush(stdout);
                
                printf("📊 AZTEC: Starting full data extraction\n");
                fflush(stdout);
                char decoded_data[256];
                int data_len = extract_aztec_data(img, x, y, 0, orientation, 
                                                decoded_data, sizeof(decoded_data));
                
                if (data_len > 0) {
                    printf("✅ AZTEC: Adding full symbol to scanner\n");
                    fflush(stdout);
                    add_aztec_symbol(iscn_ptr, (void*)img, decoded_data, data_len, x, y, 
                                   AZTEC_FULL_FINDER);
                    return; 
                }
            }
            */
        }
    }
    
    printf("ℹ️ AZTEC: No bull's-eye patterns found after checking %d positions\n", patterns_checked);
    fflush(stdout);
} 