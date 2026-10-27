/*
 * TETRIS for the Zeal 8-bit Computer.
 *
 * A compact port of the game logic from the Go/Fyne version.  The display is
 * the ZVB 640x480 4bpp graphics mode (40x30 tiles of 16x16 pixels).  One
 * beveled block tile is recoloured through palette groups, and a 5x7 ROM font
 * is expanded into 16x16 font tiles at startup.
 *
 * Controls:
 *   arrows      move / soft drop
 *   up or X     rotate clockwise
 *   space       hard drop
 *   Enter       pause / resume
 *   W           restart (when paused or game over)
 *   Q           quit to the OS
 */

#include <stdint.h>
#include <string.h>

#include <zos_sys.h>
#include <zos_vfs.h>
#include <zos_time.h>
#include <zos_keyboard.h>
#include <zvb_gfx.h>
#include <zvb_hardware.h>

#include "zgdk/ascii.h"
#include "zgdk/tilemap.h"
#include "zgdk/utils/rand.h"
#include "zgdk/input.h"
#include "zgdk/input/controller.h"
#include "zgdk/sound.h"

#if !defined(put_u8)
/* Stubs for zgdk/log_error when pulling input/controller modules */
void put_u8(unsigned char c) __naked
{
    (void)c;
    __asm
        ret
    __endasm;
}
#endif

#if !defined(put_s)
void put_s(const unsigned char *s) __naked
{
    (void)s;
    __asm
        ret
    __endasm;
}
#endif

#if !defined(put_hex8)
void put_hex8(unsigned char v) __naked
{
    (void)v;
    __asm
        ret
    __endasm;
}
#endif

/* ------------------------------------------------------------------------- */
/* Board and game constants                                                  */
/* ------------------------------------------------------------------------- */

#define BOARD_W 10
#define BOARD_H 20

#define PREVIEW_COUNT 4
#define DAS_DELAY     16          /* 267 ms at 60 Hz */
#define DAS_RATE      6           /* 100 ms at 60 Hz */
#define SOFT_RATE     1           /* one extra row every two frames */
#define LOCK_FRAMES   30          /* 0.5 s at 60 Hz */
#define MAX_LOCK_RESETS 15

/* Sound effects.  Durations are in frames (sound_loop is called once a frame). */
#define BEEP_FREQ_BASE   240      /* Hz at the top of the board */
#define BEEP_FREQ_STEP    40      /* Hz added per row */
#define BEEP_FRAMES        2
#define BOOM_NOISE_FREQ  500
#define BOOM_NOISE_FRAMES  7
#define BOOM_THUMP_FREQ   100
#define BOOM_THUMP_FRAMES   9

#define TILE_EMPTY  0
#define TILE_CELL   1
#define TILE_BLOCK  2
#define TILE_BORDER 3
#define FONT_BASE   16
#define FONT_COUNT  64

/* Palette group numbers (the high nibble of the layer-1 attribute). */
#define G_BG      0
#define G_I       1
#define G_O       2
#define G_T       3
#define G_S       4
#define G_Z       5
#define G_L       6
#define G_J       7
#define G_BORDER  8
#define G_WHITE   9
#define G_YELLOW 10
#define G_GREEN  11
#define G_RED    12
#define G_DIM    13

/* Screen coordinates in tiles. */
#define BOARD_X   5
#define BOARD_Y   4
#define BORDER_X  4
#define BORDER_Y  3
#define BORDER_W 12
#define BORDER_H 22

/* Shared geometry of the pause and game-over overlays. */
#define OVERLAY_X 10
#define OVERLAY_Y 10
#define OVERLAY_W 20
#define OVERLAY_H 10

/* ------------------------------------------------------------------------- */
/* Input bits (mapped from ZGDK input: keyboard+SNES controller)            */
/* ------------------------------------------------------------------------- */

#define K_LEFT  0x0001
#define K_RIGHT 0x0002
#define K_DOWN  0x0004
#define K_UP    0x0008
#define K_SPACE 0x0010
#define K_X     0x0020
#define K_PAUSE 0x0040
#define K_R     0x0080
#define K_Q     0x0100

/* ------------------------------------------------------------------------- */
/* Piece data.  A shape cell packs the x coordinate in the low nibble and    */
/* the y coordinate in the high nibble.                                      */
/* ------------------------------------------------------------------------- */

static const uint8_t PIECES[7][4][4] = {
    {   /* I */
        {0x10, 0x11, 0x12, 0x13},
        {0x03, 0x13, 0x23, 0x33},
        {0x20, 0x21, 0x22, 0x23},
        {0x00, 0x10, 0x20, 0x30}
    },
    {   /* O */
        {0x01, 0x02, 0x11, 0x12},
        {0x01, 0x02, 0x11, 0x12},
        {0x01, 0x02, 0x11, 0x12},
        {0x01, 0x02, 0x11, 0x12}
    },
    {   /* T */
        {0x01, 0x10, 0x11, 0x12},
        {0x01, 0x11, 0x12, 0x21},
        {0x10, 0x11, 0x12, 0x21},
        {0x01, 0x10, 0x11, 0x21}
    },
    {   /* S */
        {0x01, 0x02, 0x10, 0x11},
        {0x00, 0x10, 0x11, 0x21},
        {0x11, 0x12, 0x20, 0x21},
        {0x01, 0x11, 0x12, 0x22}
    },
    {   /* Z */
        {0x00, 0x01, 0x11, 0x12},
        {0x02, 0x11, 0x12, 0x21},
        {0x10, 0x11, 0x21, 0x22},
        {0x01, 0x10, 0x11, 0x20}
    },
    {   /* L */
        {0x02, 0x10, 0x11, 0x12},
        {0x00, 0x10, 0x20, 0x21},
        {0x10, 0x11, 0x12, 0x20},
        {0x01, 0x02, 0x12, 0x22}
    },
    {   /* J */
        {0x00, 0x10, 0x11, 0x12},
        {0x01, 0x02, 0x11, 0x21},
        {0x10, 0x11, 0x12, 0x22},
        {0x01, 0x11, 0x20, 0x21}
    }
};

/* Wall kicks, indexed by the destination rotation, exactly as in the Go game. */
static const int8_t KICKS_JLSTZ[4][5][2] = {
    { {0,0}, {-1,0}, {-1,1}, {0,-2}, {-1,-2} },
    { {0,0}, {-1,0}, {-1,-1}, {0,2}, {-1,2} },
    { {0,0}, {1,0}, {1,1}, {0,-2}, {1,-2} },
    { {0,0}, {1,0}, {1,-1}, {0,2}, {1,2} }
};

static const int8_t KICKS_I[4][5][2] = {
    { {0,0}, {1,0}, {-2,0}, {1,2}, {-2,-1} },
    { {0,0}, {-2,0}, {1,0}, {-2,1}, {1,-2} },
    { {0,0}, {-1,0}, {2,0}, {-1,-2}, {2,1} },
    { {0,0}, {2,0}, {-1,0}, {2,-1}, {-1,2} }
};

/* Gravity in 1/256 rows per frame, for levels 1..20. */
static const uint16_t GRAVITY[20] = {
    4, 5, 7, 9, 12, 16, 22, 32, 45, 67,
    99, 151, 235, 373, 604, 1001, 1693, 2928, 5120, 5120
};

static const uint16_t LINE_SCORE[5] = { 0, 40, 100, 300, 1200 };

/* ------------------------------------------------------------------------- */
/* Classic 5x7 font (ASCII 32..95), public-domain Adafruit GFX font.          */
/* Each glyph is five columns; bit 0 is the top row.                         */
/* ------------------------------------------------------------------------- */

static const uint8_t FONT5X7[64][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08}, {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06},
    {0x3E, 0x41, 0x5D, 0x59, 0x4E}, {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x73},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41}, {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40}
};

/* ------------------------------------------------------------------------- */
/* Palette.  Four colours per group are enough for everything on screen.     */
/* ------------------------------------------------------------------------- */

#define C(r,g,b) ((uint16_t)RGB888_TO_RGB565(r,g,b))

static const uint16_t PALETTE[14][4] = {
    { C(0,0,0),       C(32,32,48),    C(16,16,24),    C(255,255,255) }, /* background/grid */
    { C(0,80,80),     C(0,240,240),   C(120,255,255), C(255,255,255) }, /* I */
    { C(120,120,0),   C(240,240,0),   C(255,255,120), C(255,255,255) }, /* O */
    { C(80,0,120),    C(160,0,240),   C(210,120,255), C(255,255,255) }, /* T */
    { C(0,100,0),     C(0,220,0),     C(120,255,120), C(255,255,255) }, /* S */
    { C(120,0,0),     C(240,0,0),     C(255,120,120), C(255,255,255) }, /* Z */
    { C(120,80,0),    C(240,160,0),   C(255,210,120), C(255,255,255) }, /* L */
    { C(0,0,120),     C(0,0,240),     C(120,120,255), C(255,255,255) }, /* J */
    { C(0,0,0),       C(70,70,90),    C(110,110,130), C(180,180,200) }, /* border */
    { C(0,0,0),       C(230,230,230), C(160,160,160), C(255,255,255) }, /* white text */
    { C(0,0,0),       C(255,220,80),  C(180,150,40),  C(255,255,255) }, /* yellow text */
    { C(0,0,0),       C(0,220,60),    C(0,140,40),    C(255,255,255) }, /* green text */
    { C(0,0,0),       C(240,80,80),   C(160,40,40),   C(255,255,255) }, /* red text */
    { C(0,0,0),       C(150,150,160), C(90,90,100),   C(200,200,210) }  /* dim text */
};

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

gfx_context vctx;

static uint8_t board[BOARD_H][BOARD_W];

static uint8_t piece_id;       /* 0..6 */
static uint8_t piece_rot;      /* 0..3 */
static int8_t  piece_x;
static int8_t  piece_y;

static uint8_t queue[PREVIEW_COUNT];
static uint8_t queue_len;
static uint8_t bag[7];
static uint8_t bag_pos;

static uint16_t score;
static uint16_t lines_total;
static uint8_t  level;
static uint16_t gravity_acc;

static uint8_t grounded;
static uint8_t lock_elapsed;
static uint8_t lock_resets;

static uint8_t paused;
static uint8_t game_over;
static uint8_t game_over_shown;
static uint8_t pause_armed;
static uint8_t board_dirty;
static uint8_t quit;

static uint16_t keys;
static uint16_t pressed;

static uint8_t left_das, left_rep;
static uint8_t right_das, right_rep;
static uint8_t down_timer;

static uint8_t tilebuf[128];

/* ------------------------------------------------------------------------- */
/* Forward declarations                                                      */
/* ------------------------------------------------------------------------- */

static void draw_game_over_overlay(void);
static void draw_pause_overlay(void);
static void draw_stats(void);
static void draw_previews(void);
static void render_board(void);
static void reset_game(void);
static void spawn_piece(void);
static void lock_piece(void);
static void apply_gravity(void);
static void handle_input(void);
static void keyboard_reset_state(void);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void put_tile(uint8_t x, uint8_t y, uint8_t tile, uint8_t group)
{
    tilemap_place_xy(&vctx, 0, tile, x, y);
    tilemap_place_xy(&vctx, 1, (uint8_t)(group << 4), x, y);
}

static void fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                      uint8_t tile, uint8_t group)
{
    tilemap_fill(&vctx, 0, tile, x, y, w, h);
    tilemap_fill(&vctx, 1, (uint8_t)(group << 4), x, y, w, h);
}

static void draw_char(uint8_t x, uint8_t y, uint8_t c, uint8_t group)
{
    if (x >= 40 || y >= 30) {
        return;
    }
    tilemap_place_xy(&vctx, 0, ascii_to(c), x, y);
    tilemap_place_xy(&vctx, 1, (uint8_t)(group << 4), x, y);
}

static void draw_text(uint8_t x, uint8_t y, const char *s, uint8_t group)
{
    while (*s) {
        draw_char(x, y, (uint8_t)*s, group);
        x++;
        s++;
    }
}

static void draw_u16(uint8_t x, uint8_t y, uint16_t value,
                     uint8_t width, uint8_t group)
{
    uint16_t div = 1;
    uint8_t  i;
    uint8_t  started = 0;

    for (i = 1; i < width; i++) {
        div *= 10;
    }
    for (i = 0; i < width; i++) {
        uint8_t digit = (uint8_t)(value / div);
        value %= div;
        if (digit != 0 || started || div == 1) {
            draw_char(x + i, y, (uint8_t)('0' + digit), group);
            started = 1;
        } else {
            draw_char(x + i, y, ' ', group);
        }
        div /= 10;
    }
}

static void reset_repeat_timers(void)
{
    left_das = left_rep = 0;
    right_das = right_rep = 0;
    down_timer = 0;
}

/* Pitch rises as the piece gets closer to the bottom. */
static void play_beep(void)
{
    uint8_t y = (piece_y < 0) ? 0 : (uint8_t)piece_y;

    if (y >= BOARD_H) {
        y = BOARD_H - 1;
    }
    sound_play(0, (uint16_t)(BEEP_FREQ_BASE + (uint16_t)y * BEEP_FREQ_STEP),
               BEEP_FRAMES);
}

/* Short noise burst with a low thump underneath. */
static void play_explosion(void)
{
    sound_set(1, WAV_NOISE);
    sound_play(1, BOOM_NOISE_FREQ, BOOM_NOISE_FRAMES);
    sound_play(2, BOOM_THUMP_FREQ, BOOM_THUMP_FRAMES);
}

/* ------------------------------------------------------------------------- */
/* Tiles                                                                     */
/* ------------------------------------------------------------------------- */

static void tile_pixel(uint8_t x, uint8_t y, uint8_t color)
{
    uint16_t idx = ((uint16_t)y << 4) + x;
    uint8_t *p = &tilebuf[idx >> 1];
    if (idx & 1) {
        *p = (uint8_t)((*p & 0xF0) | (color & 0x0F));
    } else {
        *p = (uint8_t)((*p & 0x0F) | ((color & 0x0F) << 4));
    }
}

static void load_tile(uint8_t index)
{
    gfx_tileset_load_none(&vctx, tilebuf, 128,
                          (uint16_t)((uint16_t)index << 7), 0, 0);
}

static void make_empty_tile(void)
{
    memset(tilebuf, 0, 128);
    load_tile(TILE_EMPTY);
}

static void make_cell_tile(void)
{
    uint8_t x, y;
    memset(tilebuf, 0, 128);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            if (x == 0 || y == 0) {
                tile_pixel(x, y, 1);
            }
        }
    }
    load_tile(TILE_CELL);
}

static void make_block_tile(void)
{
    uint8_t x, y;
    memset(tilebuf, 0, 128);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            uint8_t c;
            if (x == 0 || y == 0 || x == 15 || y == 15) {
                c = 0;                      /* dark outline */
            } else if (x == 1 || y == 1) {
                c = 2;                      /* light bevel */
            } else if (x == 14 || y == 14) {
                c = 0;                      /* dark inner edge */
            } else {
                c = 1;                      /* main colour */
            }
            if (x >= 2 && x <= 3 && y >= 2 && y <= 3) {
                c = 3;                      /* highlight */
            }
            tile_pixel(x, y, c);
        }
    }
    load_tile(TILE_BLOCK);
}

static void make_border_tile(void)
{
    uint8_t x, y;
    memset(tilebuf, 0, 128);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            uint8_t c = 1;
            if (x == 0 || y == 0) {
                c = 2;
            } else if (x == 15 || y == 15) {
                c = 0;
            }
            tile_pixel(x, y, c);
        }
    }
    load_tile(TILE_BORDER);
}

static void make_font_tile(uint8_t ch)
{
    uint8_t col, row;
    memset(tilebuf, 0, 128);
    for (col = 0; col < 5; col++) {
        uint8_t bits = FONT5X7[ch][col];
        for (row = 0; row < 7; row++) {
            if (bits & (uint8_t)(1 << row)) {
                uint8_t px = (uint8_t)(3 + (col << 1));
                uint8_t py = (uint8_t)(1 + (row << 1));
                tile_pixel(px,     py,     1);
                tile_pixel(px + 1, py,     1);
                tile_pixel(px,     py + 1, 1);
                tile_pixel(px + 1, py + 1, 1);
            }
        }
    }
    load_tile((uint8_t)(FONT_BASE + ch));
}

static void load_tiles(void)
{
    uint8_t i;
    make_empty_tile();
    make_cell_tile();
    make_block_tile();
    make_border_tile();
    for (i = 0; i < FONT_COUNT; i++) {
        make_font_tile(i);
    }
    ascii_map(32, FONT_COUNT, FONT_BASE);
}

static void load_palette(void)
{
    uint8_t i;
    for (i = 0; i < 14; i++) {
        gfx_palette_load(&vctx, (void *)PALETTE[i], 8,
                         (uint8_t)(i << 4));
    }
}

/* ------------------------------------------------------------------------- */
/* Static screen furniture                                                   */
/* ------------------------------------------------------------------------- */

static void draw_static_ui(void)
{
    draw_text(7, 1, "TETRIS", G_YELLOW);
    fill_rect(BORDER_X, BORDER_Y, BORDER_W, BORDER_H, TILE_BORDER, G_BORDER);

    draw_text(17, 4, "SCORE", G_WHITE);
    draw_text(17, 9, "LEVEL", G_WHITE);
    draw_text(17, 14, "LINES", G_WHITE);
    draw_text(27, 4, "NEXT", G_WHITE);

    draw_text(4, 26, "ARROWS MOVE  UP ROTATE", G_DIM);
    draw_text(4, 27, "SPACE DROP  ENTER PAUSE", G_DIM);
    draw_text(36, 28, "2026", G_DIM);
}

static void clear_overlay(void)
{
    fill_rect(OVERLAY_X, OVERLAY_Y, OVERLAY_W, OVERLAY_H,
              TILE_EMPTY, G_BG);
}

/* The tilemap has no backing buffer: redraw everything an overlay may hide. */
static void restore_scene(void)
{
    clear_overlay();
    draw_static_ui();
    render_board();
    draw_previews();
    draw_stats();
    board_dirty = 0;
}

/* ------------------------------------------------------------------------- */
/* Game state                                                                */
/* ------------------------------------------------------------------------- */

static uint8_t collision(int8_t nx, int8_t ny, const uint8_t *shape)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        int8_t x = (int8_t)(nx + (shape[i] & 0x0F));
        int8_t y = (int8_t)(ny + (shape[i] >> 4));
        if (x < 0 || x >= BOARD_W || y >= BOARD_H) {
            return 1;
        }
        if (y >= 0 && board[y][x] != 0) {
            return 1;
        }
    }
    return 0;
}

static void shuffle_bag(void)
{
    uint8_t i;
    for (i = 0; i < 7; i++) {
        bag[i] = i;
    }
    for (i = 6; i > 0; i--) {
        uint8_t j = (uint8_t)((uint8_t)rand8() % (uint8_t)(i + 1));
        uint8_t t = bag[i];
        bag[i] = bag[j];
        bag[j] = t;
    }
}

static void fill_queue(void)
{
    while (queue_len < PREVIEW_COUNT) {
        if (bag_pos >= 7) {
            shuffle_bag();
            bag_pos = 0;
        }
        queue[queue_len++] = bag[bag_pos++];
    }
}

static uint8_t queue_pop(void)
{
    uint8_t i;
    uint8_t id = queue[0];
    for (i = 0; (uint8_t)(i + 1) < queue_len; i++) {
        queue[i] = queue[i + 1];
    }
    if (queue_len != 0) {
        queue_len--;
    }
    fill_queue();
    return id;
}

static void reset_lock_delay(void)
{
    if (!grounded || lock_resets >= MAX_LOCK_RESETS) {
        return;
    }
    lock_elapsed = 0;
    lock_resets++;
}

static void spawn_piece(void)
{
    piece_id = queue_pop();
    piece_x = 4;
    piece_y = 0;
    piece_rot = 0;
    gravity_acc = 0;
    grounded = 0;
    lock_elapsed = 0;
    lock_resets = 0;

    if (collision(piece_x, piece_y, PIECES[piece_id][0])) {
        game_over = 1;
    }
    draw_previews();
    board_dirty = 1;
}

static uint8_t clear_lines(void)
{
    uint8_t cleared = 0;
    int8_t y = BOARD_H - 1;
    while (y >= 0) {
        uint8_t x;
        uint8_t full = 1;
        for (x = 0; x < BOARD_W; x++) {
            if (board[y][x] == 0) {
                full = 0;
                break;
            }
        }
        if (full) {
            int8_t yy;
            for (yy = y; yy > 0; yy--) {
                for (x = 0; x < BOARD_W; x++) {
                    board[yy][x] = board[yy - 1][x];
                }
            }
            for (x = 0; x < BOARD_W; x++) {
                board[0][x] = 0;
            }
            cleared++;
            /* stay on the same row and re-check the shifted line */
        } else {
            y--;
        }
    }
    return cleared;
}

static void lock_piece(void)
{
    uint8_t i, cleared;
    const uint8_t *shape = PIECES[piece_id][piece_rot];

    for (i = 0; i < 4; i++) {
        int8_t x = (int8_t)(piece_x + (shape[i] & 0x0F));
        int8_t y = (int8_t)(piece_y + (shape[i] >> 4));
        if (x >= 0 && x < BOARD_W && y >= 0 && y < BOARD_H) {
            board[y][x] = (uint8_t)(piece_id + 1);
        }
    }

    cleared = clear_lines();
    if (cleared != 0) {
        uint16_t base = LINE_SCORE[cleared > 4 ? 4 : cleared];
        uint16_t mult = (uint16_t)(level + 1);
        uint16_t add;
        play_explosion();
        if (mult > 40) {
            mult = 40;                  /* keep the 16-bit product in range */
        }
        add = (uint16_t)(base * mult);
        if (score > (uint16_t)(65535 - add)) {
            score = 65535;
        } else {
            score += add;
        }
        lines_total += cleared;
        level = (uint8_t)(lines_total / 10 + 1);
    }

    /* Hard drop adds score outside this function, so refresh the HUD even
     * when no line was cleared. */
    draw_stats();
    spawn_piece();
    board_dirty = 1;
}

static void reset_game(void)
{
    uint8_t x, y;

    for (y = 0; y < BOARD_H; y++) {
        for (x = 0; x < BOARD_W; x++) {
            board[y][x] = 0;
        }
    }

    score = 0;
    lines_total = 0;
    level = 1;
    gravity_acc = 0;
    grounded = 0;
    lock_elapsed = 0;
    lock_resets = 0;
    paused = 0;
    game_over = 0;
    game_over_shown = 0;
    reset_repeat_timers();
    board_dirty = 1;

    bag_pos = 7;
    queue_len = 0;
    fill_queue();
    spawn_piece();

    draw_stats();
}

/* ------------------------------------------------------------------------- */
/* Movement and gravity                                                      */
/* ------------------------------------------------------------------------- */

static void move_horizontal(int8_t dx)
{
    if (game_over || paused) {
        return;
    }
    if (!collision((int8_t)(piece_x + dx), piece_y,
                   PIECES[piece_id][piece_rot])) {
        piece_x = (int8_t)(piece_x + dx);
        reset_lock_delay();
        board_dirty = 1;
    }
}

static void move_down(void)
{
    if (game_over || paused) {
        return;
    }
    if (!collision(piece_x, (int8_t)(piece_y + 1),
                   PIECES[piece_id][piece_rot])) {
        piece_y++;
        grounded = 0;
        lock_elapsed = 0;
        board_dirty = 1;
        play_beep();
    }
}

static void rotate_piece(void)
{
    uint8_t rot;
    uint8_t i;
    const int8_t (*kicks)[2];

    if (game_over || paused || piece_id == 1) {
        return;
    }

    rot = (uint8_t)((piece_rot + 1) & 3);
    kicks = (piece_id == 0) ? KICKS_I[rot] : KICKS_JLSTZ[rot];

    for (i = 0; i < 5; i++) {
        int8_t nx = (int8_t)(piece_x + kicks[i][0]);
        int8_t ny = (int8_t)(piece_y + kicks[i][1]);
        if (!collision(nx, ny, PIECES[piece_id][rot])) {
            piece_x = nx;
            piece_y = ny;
            piece_rot = rot;
            reset_lock_delay();
            board_dirty = 1;
            return;
        }
    }
}

static void hard_drop(void)
{
    if (game_over || paused) {
        return;
    }
    while (!collision(piece_x, (int8_t)(piece_y + 1),
                      PIECES[piece_id][piece_rot])) {
        piece_y++;
        if (score > 65533) {
            score = 65535;
        } else {
            score += 2;
        }
    }
    play_beep();
    lock_piece();
}

static void apply_gravity(void)
{
    uint8_t idx;
    uint8_t moved = 0;
    uint8_t on_stack;

    if (game_over || paused) {
        return;
    }

    idx = (level > 20) ? 19 : (uint8_t)(level - 1);
    gravity_acc += GRAVITY[idx];

    while (gravity_acc >= 256) {
        if (collision(piece_x, (int8_t)(piece_y + 1),
                      PIECES[piece_id][piece_rot])) {
            gravity_acc = 0;
            break;
        }
        piece_y++;
        gravity_acc -= 256;
        moved = 1;
    }

    on_stack = collision(piece_x, (int8_t)(piece_y + 1),
                         PIECES[piece_id][piece_rot]);

    if (moved) {
        play_beep();
        grounded = on_stack;
        lock_elapsed = 0;
        board_dirty = 1;
        return;
    }
    if (!on_stack) {
        grounded = 0;
        lock_elapsed = 0;
        return;
    }
    if (!grounded) {
        grounded = 1;
        lock_elapsed = 0;
        return;
    }

    lock_elapsed++;
    if (lock_elapsed < LOCK_FRAMES) {
        return;
    }

    lock_elapsed = 0;
    grounded = 0;
    lock_resets = 0;
    lock_piece();
    board_dirty = 1;
}

/* ------------------------------------------------------------------------- */
/* Input (keyboard + SNES controller via ZGDK)                               */
/* ------------------------------------------------------------------------- */

static void keyboard_reset_state(void)
{
    keys = 0;
    pressed = 0;
    reset_repeat_timers();
    pause_armed = 1;
}

static void read_input(void)
{
    uint16_t in = input_get();
    uint16_t cur = 0;
    uint16_t old_keys = keys;

    pressed = 0;

    /* Movement */
    if (in & BUTTON_LEFT)  cur |= K_LEFT;
    if (in & BUTTON_RIGHT) cur |= K_RIGHT;
    if (in & BUTTON_DOWN)  cur |= K_DOWN;

    /* Rotate: Up / A / X / Y */
    if (in & BUTTON_UP) cur |= K_X;
    if (in & BUTTON_A)  cur |= K_X;
    if (in & BUTTON_X)  cur |= K_X;
    if (in & BUTTON_Y)  cur |= K_X;

    /* Hard drop: B = Space/Z in ZGDK mapping */
    if (in & BUTTON_B) cur |= K_SPACE;

    /* Pause: Start = Enter */
    if (in & BUTTON_START) cur |= K_PAUSE;

    /* Restart: R button = W on keyboard */
    if (in & BUTTON_R) cur |= K_R;

    /* Quit: L button = Q on keyboard */
    if (in & BUTTON_L) cur |= K_Q;

    pressed = (uint16_t)(cur & (uint16_t)~old_keys);
    keys = cur;

    /* Only allow another pause toggle after Enter/Start was released. */
    if (!(keys & K_PAUSE)) {
        pause_armed = 1;
    }
}

static void handle_input(void)
{
    if (pressed & K_Q) {
        if (paused || game_over) {
            quit = 1;
            keyboard_reset_state();
            pressed = 0;
            return;
        }
    }

    if (pressed & K_R) {
        if (paused || game_over) {
            reset_game();
            restore_scene();
            pressed = 0;
            return;
        }
    }

    if (pressed & K_PAUSE) {
        if (game_over) {
            pressed = 0;
            return;
        }
        if (!pause_armed) {
            pressed = 0;
            return;
        }
        pause_armed = 0;
        if (paused) {
            paused = 0;
            reset_repeat_timers();
            restore_scene();
        } else {
            paused = 1;
            draw_pause_overlay();
        }
        pressed = 0;
        return;
    }

    if (game_over || paused) {
        pressed = 0;
        return;
    }

    if (pressed & K_X) {
        rotate_piece();
    }
    if (pressed & K_SPACE) {
        hard_drop();
    }

    if (pressed & K_LEFT) {
        move_horizontal(-1);
        left_das = 1;
        left_rep = 0;
    } else if (keys & K_LEFT) {
        if (left_das < DAS_DELAY) {
            left_das++;
        } else {
            if (left_rep == 0) {
                move_horizontal(-1);
                left_rep = DAS_RATE - 1;
            } else {
                left_rep--;
            }
        }
    } else {
        left_das = 0;
        left_rep = 0;
    }

    if (pressed & K_RIGHT) {
        move_horizontal(1);
        right_das = 1;
        right_rep = 0;
    } else if (keys & K_RIGHT) {
        if (right_das < DAS_DELAY) {
            right_das++;
        } else {
            if (right_rep == 0) {
                move_horizontal(1);
                right_rep = DAS_RATE - 1;
            } else {
                right_rep--;
            }
        }
    } else {
        right_das = 0;
        right_rep = 0;
    }

    if (pressed & K_DOWN) {
        move_down();
        down_timer = SOFT_RATE;
    } else if (keys & K_DOWN) {
        if (down_timer != 0) {
            down_timer--;
        } else {
            move_down();
            down_timer = SOFT_RATE;
        }
    } else {
        down_timer = 0;
    }

    pressed = 0;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static void draw_stats(void)
{
    draw_u16(17, 6, score, 5, G_GREEN);
    draw_u16(17, 11, level, 3, G_GREEN);
    draw_u16(17, 16, lines_total, 5, G_GREEN);
}

static void draw_previews(void)
{
    uint8_t i;
    for (i = 0; i < PREVIEW_COUNT; i++) {
        uint8_t px = 27;
        uint8_t py = (uint8_t)(6 + (i * 5));
        uint8_t id = queue[i];
        uint8_t ox = 0;
        uint8_t oy = (id == 0) ? 0 : 1;
        uint8_t c;
        fill_rect(px, py, 4, 4, TILE_CELL, G_BG);
        for (c = 0; c < 4; c++) {
            uint8_t nx = (uint8_t)((PIECES[id][0][c] & 0x0F) + ox);
            uint8_t ny = (uint8_t)((PIECES[id][0][c] >> 4) + oy);
            if (nx < 4 && ny < 4) {
                put_tile((uint8_t)(px + nx), (uint8_t)(py + ny),
                         TILE_BLOCK, (uint8_t)(id + 1));
            }
        }
    }
}

static void render_board(void)
{
    int8_t y;
    uint8_t x;
    uint8_t tiles[BOARD_W];
    uint8_t attrs[BOARD_W];

    for (y = BOARD_H - 1; y >= 0; y--) {
        for (x = 0; x < BOARD_W; x++) {
            uint8_t v = board[y][x];
            if (v != 0) {
                tiles[x] = TILE_BLOCK;
                attrs[x] = (uint8_t)(v << 4);
            } else {
                tiles[x] = TILE_CELL;
                attrs[x] = 0;
            }
        }
        gfx_tilemap_load(&vctx, tiles, BOARD_W, 0,
                         BOARD_X, (uint8_t)(BOARD_Y + y));
        gfx_tilemap_load(&vctx, attrs, BOARD_W, 1,
                         BOARD_X, (uint8_t)(BOARD_Y + y));
    }

    if (!game_over) {
        const uint8_t *shape = PIECES[piece_id][piece_rot];
        for (x = 0; x < 4; x++) {
            int8_t px = (int8_t)(piece_x + (shape[x] & 0x0F));
            int8_t py = (int8_t)(piece_y + (shape[x] >> 4));
            if (px >= 0 && px < BOARD_W && py >= 0 && py < BOARD_H) {
                put_tile((uint8_t)(BOARD_X + px), (uint8_t)(BOARD_Y + py),
                         TILE_BLOCK, (uint8_t)(piece_id + 1));
            }
        }
    }
}

static void draw_pause_overlay(void)
{
    /* Yellow border around the shared overlay area */
    fill_rect(OVERLAY_X, OVERLAY_Y, OVERLAY_W, OVERLAY_H, TILE_BORDER, G_YELLOW);
    /* Black background inside */
    fill_rect(OVERLAY_X + 1, OVERLAY_Y + 1, OVERLAY_W - 2, OVERLAY_H - 2,
              TILE_EMPTY, G_BG);
    /* Title */
    draw_text(17, 12, "PAUSE", G_YELLOW);
    /* Menu items */
    draw_text(13, 14, "ENTER - RESUME", G_WHITE);
    draw_text(13, 15, "W - RESTART", G_WHITE);
    draw_text(13, 16, "Q - QUIT TO OS", G_WHITE);
}

static void draw_game_over_overlay(void)
{
    /* Red border around the shared overlay area */
    fill_rect(OVERLAY_X, OVERLAY_Y, OVERLAY_W, OVERLAY_H, TILE_BORDER, G_RED);
    /* Black background inside */
    fill_rect(OVERLAY_X + 1, OVERLAY_Y + 1, OVERLAY_W - 2, OVERLAY_H - 2,
              TILE_EMPTY, G_BG);
    /* Title */
    draw_text(15, 12, "GAME OVER", G_RED);
    /* Menu items */
    draw_text(12, 14, "W - RESTART", G_WHITE);
    draw_text(12, 16, "Q - QUIT TO OS", G_WHITE);
}

/* ------------------------------------------------------------------------- */
/* Initialisation and main loop                                              */
/* ------------------------------------------------------------------------- */

static void init_video(void)
{
    zvb_ctrl_l0_scr_x_low  = 0;
    zvb_ctrl_l0_scr_x_high = 0;
    zvb_ctrl_l0_scr_y_low  = 0;
    zvb_ctrl_l0_scr_y_high = 0;
    zvb_ctrl_l1_scr_x_low  = 0;
    zvb_ctrl_l1_scr_x_high = 0;
    zvb_ctrl_l1_scr_y_low  = 0;
    zvb_ctrl_l1_scr_y_high = 0;

    gfx_initialize(ZVB_CTRL_VID_MODE_GFX_640_4BIT, &vctx);
    gfx_enable_screen(1);

    load_palette();
    load_tiles();

    /* Clear both tilemap layers; gfx_initialize only clears layer 0. */
    tilemap_fill(&vctx, 0, TILE_EMPTY, 0, 0, 40, 30);
    tilemap_fill(&vctx, 1, 0, 0, 0, 40, 30);

    draw_static_ui();
}

int main(void)
{
    zos_time_t now;

    if (gettime(0, &now) == ERR_SUCCESS && now.t_millis != 0) {
        rand8_seed(now.t_millis);
    } else {
        rand8_seed(0xACE1);
    }

    input_init(1);
    sound_init();

    init_video();
    keyboard_reset_state();
    reset_game();

    while (!quit) {
        read_input();
        handle_input();

        if (!quit && !paused && !game_over) {
            apply_gravity();
            if (board_dirty) {
                render_board();
                board_dirty = 0;
            }
        }

        /* Draw game over only after the final board render, otherwise the
         * board would overwrite the left side of the overlay. */
        if (game_over && !game_over_shown) {
            if (board_dirty) {
                render_board();
                board_dirty = 0;
            }
            draw_game_over_overlay();
            game_over_shown = 1;
        }

        sound_loop();
        gfx_wait_vblank(&vctx);
    }

    sound_deinit();

    /* Perform system reset by jumping to 0x0000 */
    __asm
        jp 0x0000
    __endasm;

    return 0;
}
