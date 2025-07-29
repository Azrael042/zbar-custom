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
#ifndef _AZTEC_H_
#define _AZTEC_H_

/* Aztec code constants */
#define AZTEC_MIN_QUIET_ZONE    4       /* minimum quiet zone width */
#define AZTEC_COMPACT_FINDER    9       /* compact bull's-eye size */
#define AZTEC_FULL_FINDER       13      /* full bull's-eye size */
#define AZTEC_COMPACT_CORE      11      /* compact core size */
#define AZTEC_FULL_CORE         15      /* full core size */
#define AZTEC_MAX_LAYERS        32      /* maximum data layers */
#define AZTEC_MIN_SIZE          15      /* minimum symbol size */
#define AZTEC_MAX_SIZE          151     /* maximum symbol size */

/* Aztec orientation corner patterns */
#define AZTEC_CORNER_UL         0x7     /* upper left: 3 black (111) */
#define AZTEC_CORNER_UR         0x3     /* upper right: 1 white + 2 black (011) */
#define AZTEC_CORNER_LR         0x4     /* lower right: 1 black + 2 white (100) */
#define AZTEC_CORNER_LL         0x0     /* lower left: 3 white (000) */

/* Aztec decoder states */
typedef enum {
    AZTEC_STATE_INIT,           /* initial state */
    AZTEC_STATE_FINDER,         /* looking for finder pattern */
    AZTEC_STATE_ORIENTATION,    /* determining orientation */
    AZTEC_STATE_MODE,           /* reading mode message */
    AZTEC_STATE_DATA,           /* reading data layers */
    AZTEC_STATE_COMPLETE        /* decoding complete */
} aztec_state_t;

/* Aztec finder pattern info */
typedef struct aztec_finder_s {
    int center_x, center_y;     /* center coordinates */
    int size;                   /* finder pattern size (9 or 13) */
    int is_compact;             /* 1 if compact, 0 if full */
    int orientation;            /* orientation (0-3) */
} aztec_finder_t;

/* Aztec mode message info */
typedef struct aztec_mode_s {
    int layers;                 /* number of data layers */
    int data_codewords;         /* number of data codewords */
    int ecc_codewords;          /* number of error correction codewords */
} aztec_mode_t;

/* Aztec specific decode state */
typedef struct aztec_decoder_s {
    unsigned direction : 1;     /* scan direction: 0=fwd, 1=rev */
    unsigned element   : 4;     /* element offset */
    int character      : 12;    /* character position in symbol */
    unsigned width;             /* current character width */
    
    aztec_state_t state;        /* current decoder state */
    aztec_finder_t finder;      /* finder pattern info */
    aztec_mode_t mode;          /* mode message info */
    
    int pattern_buffer[7];      /* buffer for pattern matching */
    int pattern_idx;            /* current pattern index */
    int bit_count;              /* bit accumulator */
    unsigned char *data_buffer; /* decoded data buffer */
    int data_length;            /* length of decoded data */
    
    unsigned config;
    int configs[NUM_CFGS];      /* int valued configurations */
} aztec_decoder_t;

/* reset Aztec specific state */
static inline void aztec_reset(aztec_decoder_t *dcode_aztec)
{
    dcode_aztec->direction = 0;
    dcode_aztec->element   = 0;
    dcode_aztec->character = -1;
    dcode_aztec->width     = 0;
    dcode_aztec->state     = AZTEC_STATE_INIT;
    dcode_aztec->pattern_idx = 0;
    dcode_aztec->bit_count = 0;
    dcode_aztec->data_length = 0;
    
    /* Reset finder pattern info */
    dcode_aztec->finder.center_x = 0;
    dcode_aztec->finder.center_y = 0;
    dcode_aztec->finder.size = 0;
    dcode_aztec->finder.is_compact = 0;
    dcode_aztec->finder.orientation = 0;
    
    /* Reset mode message info */
    dcode_aztec->mode.layers = 0;
    dcode_aztec->mode.data_codewords = 0;
    dcode_aztec->mode.ecc_codewords = 0;
}

/* decode Aztec symbols */
zbar_symbol_type_t _zbar_decode_aztec(zbar_decoder_t *dcode);

#endif 