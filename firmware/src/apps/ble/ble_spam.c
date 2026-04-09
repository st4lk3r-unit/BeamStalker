/*
 * ble_spam.c - BLE spam UI (thin wrapper over ble_spam_svc).
 *
 * Modes: APPLE, SAMSUNG, GOOGLE, NAME, ALL.
 * All TX logic lives in ble_spam_svc.c.
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "apps/ble/ble_spam.h"
#include "apps/ble/ble_spam_svc.h"
#include "apps/ble/ble_common.h"

#include "bs/bs_arch.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"

#include <stdio.h>

/* ── Rate options ────────────────────────────────────────────────────────── */

typedef enum { INTERVAL_FAST=0, INTERVAL_NORMAL, INTERVAL_SLOW, INTERVAL_COUNT } spam_ivl_t;
static const uint32_t    k_intervals_ms[] = { 20, 100, 500 };
static const char* const k_ivl_names[]    = { "Fast", "Normal", "Slow" };

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_spam(spam_ivl_t ivl) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    int sw = bs_gfx_width();
    int cy = bs_ui_content_y(), lh = bs_gfx_text_h(ts) + 6;
    char buf[32];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("BLE Spam");

    int y = cy;
    bs_gfx_text(8, y, "RUNNING", g_bs_theme.accent, ts); y += lh;

    snprintf(buf, sizeof buf, "Mode : %s", k_ble_spam_mode_names[ble_spam_svc_mode()]);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts, true); y += lh;

    snprintf(buf, sizeof buf, "Rate : %s (%ums)", k_ivl_names[ivl],
             (unsigned)k_intervals_ms[ivl]);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts, true); y += lh;

    snprintf(buf, sizeof buf, "Sent : %lu", (unsigned long)ble_spam_svc_count());
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.dim, ts2, true);

    bs_ui_draw_hint("SEL=mode  UP/DN=rate  BACK=exit");
    bs_gfx_present();
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void ble_spam_run(const bs_arch_t* arch) {
    spam_ivl_t ivl = INTERVAL_NORMAL;

    ble_spam_svc_init(arch);
    ble_spam_svc_start(BLE_SPAM_APPLE, k_intervals_ms[ivl]);

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_SELECT: {
                    ble_spam_mode_t m = (ble_spam_mode_t)
                        ((ble_spam_svc_mode() + 1) % BLE_SPAM_MODE_COUNT);
                    ble_spam_svc_set_mode(m);
                    break;
                }
                case BS_NAV_UP: case BS_NAV_PREV:
                    ivl = (spam_ivl_t)((ivl + INTERVAL_COUNT - 1) % INTERVAL_COUNT);
                    ble_spam_svc_set_interval(k_intervals_ms[ivl]);
                    break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    ivl = (spam_ivl_t)((ivl + 1) % INTERVAL_COUNT);
                    ble_spam_svc_set_interval(k_intervals_ms[ivl]);
                    break;
                case BS_NAV_BACK:
                    ble_spam_svc_stop();
                    return;
                default: break;
            }
        }

        ble_spam_svc_tick(now);
        draw_spam(ivl);   /* always redraw — carousel needs it */
        arch->delay_ms(16);
    }
}

#endif /* BS_HAS_BLE */
