/*
 * bs_demo.c - DVD-bounce idle animation.
 *
 * BEAMSTALKER banner drifts and bounces around the screen.
 * Uses the SGFX FB+presenter pattern (via bs_gfx):
 *   1. Erase old position (fill black rectangle)
 *   2. Update position vector
 *   3. Draw at new position
 *   4. bs_gfx_present() - only dirty tiles sent over bus → fast
 *
 * Palette cycles on each wall bounce:
 *   BS_COL_FG → BS_COL_BRIGHT → BS_COL_ACCENT → BS_COL_DIM → (repeat)
 *
 * Scale panel (bottom-right corner):
 *   Animates the skull through a sequence of downscale steps so the
 *   OR-block rescaling quality is visible at each level.
 *   Ping-pong sequence: step 2 → 3 → 4 → 5 → 6 → 8 → 6 → 5 → 4 → 3 → …
 */
#include "bs_demo.h"
#include "bs/bs_gfx.h"
#include "bs/bs_keys.h"
#include "bs/bs_assets.h"

#define SKULL_W   120
#define SKULL_H   120
#define DX_INIT     2
#define DY_INIT     1

/* Frame delay in ms - target ~30 fps on native; hardware drives faster */
#define FRAME_MS  33

/* Palette cycle - four shades of orange/amber */
static const bs_color_t k_palette[] = {
    {0xFF, 0x77, 0x00, 0xFF},   /* BS_COL_FG     - amber orange */
    {0xFF, 0xAA, 0x22, 0xFF},   /* BS_COL_BRIGHT - bright amber */
    {0xFF, 0xCC, 0x44, 0xFF},   /* BS_COL_ACCENT - pale gold    */
    {0x80, 0x38, 0x00, 0xFF},   /* BS_COL_DIM    - dark orange  */
};
#define PALETTE_LEN 4

/* ---- Scale panel -------------------------------------------------------- */
/*
 * Panel layout (all coords relative to panel top-left px,py):
 *   1px  top border
 *   64px skull render area (64×64, skull centred inside)
 *   2px  gap
 *   7px  scale-factor label  e.g. "1/4x"
 *   1px  bottom border
 *   = 75px total panel height
 *   Panel width: 70px
 */
#define PNL_W         70
#define PNL_SKULL_SZ  64   /* inner skull cell */
#define PNL_TXT_H      7   /* 1-px-scale font */
#define PNL_H         (1 + PNL_SKULL_SZ + 2 + PNL_TXT_H + 1 + 2)
#define PNL_PAD        4   /* gap from screen edge */

/* Ping-pong step sequence: shrinks then grows */
static const int k_pan_steps[]  = {2, 3, 4, 5, 6, 8, 6, 5, 4, 3};
static const char* k_pan_labels[] = {"1/2x","1/3x","1/4x","1/5x","1/6x",
                                     "1/8x","1/6x","1/5x","1/4x","1/3x"};
#define PAN_SEQ_LEN   10
#define PAN_FRAMES    22   /* frames per step (~0.7 s at 30 fps) */

static int s_pan_frame;
static int s_pan_idx;

/* Cached panel position (set once in init) */
static int s_px, s_py;

static void erase_panel(void) {
    bs_gfx_fill_rect(s_px - 1, s_py - 1, PNL_W + 2, PNL_H + 2, BS_COL_BG);
}

static void draw_panel(void) {
    int step       = k_pan_steps[s_pan_idx];
    int skull_size = (SKULL_W + step - 1) / step;   /* ceil(120/step) */

    /* Background + border lines */
    bs_gfx_fill_rect(s_px, s_py, PNL_W, PNL_H, BS_COL_BG);
    bs_gfx_hline(s_px, s_py,           PNL_W, BS_COL_DIM);
    bs_gfx_hline(s_px, s_py + PNL_H - 1, PNL_W, BS_COL_DIM);

    /* Skull centred in the 64×64 cell */
    int cell_x = s_px + (PNL_W - PNL_SKULL_SZ) / 2;
    int cell_y = s_py + 1;
    int ox = cell_x + (PNL_SKULL_SZ - skull_size) / 2;
    int oy = cell_y + (PNL_SKULL_SZ - skull_size) / 2;
    bs_gfx_bitmap_1bpp(ox, oy, SKULL_W, SKULL_H, bs_skull_120, BS_COL_FG, 1, step);

    /* Scale-factor label, centred below skull */
    const char* lbl = k_pan_labels[s_pan_idx];
    int lw = bs_gfx_text_w(lbl, 1);
    int lx = s_px + (PNL_W - lw) / 2;
    int ly = s_py + 1 + PNL_SKULL_SZ + 2;
    bs_gfx_text(lx, ly, lbl, BS_COL_DIM, 1);

    /* Advance animation */
    if (++s_pan_frame >= PAN_FRAMES) {
        s_pan_frame = 0;
        s_pan_idx   = (s_pan_idx + 1) % PAN_SEQ_LEN;
    }
}

/* ---- Bounce state ------------------------------------------------------- */
static int s_x, s_y;        /* current top-left position */
static int s_dx, s_dy;      /* velocity */
static int s_tw, s_th;      /* text bounding box */
static int s_sw, s_sh;      /* screen dimensions  */
static int s_pal;           /* palette index      */

static void erase_banner(void) {
    bs_gfx_fill_rect(s_x - 1, s_y - 1, SKULL_W + 2, SKULL_H + 2, BS_COL_BG);
}

static void draw_banner(void) {
    bs_gfx_bitmap_1bpp(s_x, s_y, SKULL_W, SKULL_H, bs_skull_120, k_palette[s_pal], 1, 1);
}

/* ---- public ----------------------------------------------------------- */

void bs_demo_init(const bs_arch_t* arch) {
    (void)arch;
    s_sw = bs_gfx_width();
    s_sh = bs_gfx_height();
    s_tw = SKULL_W;
    s_th = SKULL_H;

    /* Start centred */
    s_x  = (s_sw - s_tw) / 2;
    s_y  = (s_sh - s_th) / 2;
    s_dx = DX_INIT;
    s_dy = DY_INIT;
    s_pal = 0;

    /* Scale panel - bottom-right corner */
    s_px = s_sw - PNL_W - PNL_PAD;
    s_py = s_sh - PNL_H - PNL_PAD;
    s_pan_frame = 0;
    s_pan_idx   = 0;

    bs_gfx_clear(BS_COL_BG);
    draw_banner();
    draw_panel();
    /* present handled by bs_run() so debug overlay can be drawn on top */
}

bool bs_demo_tick(const bs_arch_t* arch) {
    /* Check for exit key */
    bs_key_t key;
    if (bs_keys_poll(&key)) {
        if (key.id != BS_KEY_NONE) return true;
    }

    /* Erase both elements before updating */
    erase_banner();
    erase_panel();

    /* Update bounce position */
    s_x += s_dx;
    s_y += s_dy;

    /* Bounce: check X edges */
    bool bounced = false;
    if (s_x <= 0) {
        s_x  = 0;
        s_dx = (s_dx < 0) ? -s_dx : s_dx;
        bounced = true;
    } else if (s_x + s_tw >= s_sw) {
        s_x  = s_sw - s_tw;
        s_dx = (s_dx > 0) ? -s_dx : s_dx;
        bounced = true;
    }

    /* Bounce: check Y edges */
    if (s_y <= 0) {
        s_y  = 0;
        s_dy = (s_dy < 0) ? -s_dy : s_dy;
        bounced = true;
    } else if (s_y + s_th >= s_sh) {
        s_y  = s_sh - s_th;
        s_dy = (s_dy > 0) ? -s_dy : s_dy;
        bounced = true;
    }

    /* Cycle palette on bounce */
    if (bounced) {
        s_pal = (s_pal + 1) % PALETTE_LEN;
    }

    /* Redraw - panel on top so it's always fully visible */
    draw_banner();
    draw_panel();
    /* present handled by bs_run() after debug overlay is drawn */

    arch->delay_ms(FRAME_MS);
    return false;
}
