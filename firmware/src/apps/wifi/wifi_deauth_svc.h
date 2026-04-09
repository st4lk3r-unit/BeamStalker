#pragma once
/*
 * wifi_deauth_svc.h - Deauth attack service layer.
 *
 * State machine: IDLE → SCANNING → AP_READY → SNIFFING → CLIENT_READY
 *                → ATTACKING → DONE
 *
 * Callers (UI or CLI):
 *   - Call deauth_svc_init() once before use.
 *   - Drive deauth_svc_tick(now_ms) every loop iteration.
 *   - Use transition functions to move between states.
 *   - Read accessors to display progress.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"
#include "wifi_common.h"

/* ── State ───────────────────────────────────────────────────────────────── */

typedef enum {
    DEAUTH_SVC_IDLE = 0,
    DEAUTH_SVC_SCANNING,      /* AP scan running                               */
    DEAUTH_SVC_AP_READY,      /* APs discovered, waiting for selection         */
    DEAUTH_SVC_SNIFFING,      /* passive client discovery running              */
    DEAUTH_SVC_CLIENT_READY,  /* clients ready (or sniff skipped)              */
    DEAUTH_SVC_ATTACKING,     /* deauth loop active                            */
    DEAUTH_SVC_DONE,          /* attack stopped                                */
} deauth_svc_state_t;

/* ── Activity log ────────────────────────────────────────────────────────── */

#define DEAUTH_SVC_LOG_LINES   8
#define DEAUTH_SVC_LOG_LEN    48

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void deauth_svc_init(const bs_arch_t* arch);

/* ── Transitions ─────────────────────────────────────────────────────────── */

void deauth_svc_scan_aps(void);       /* → SCANNING                           */
/* Shortcut for CLI: no scan/select — broadcast flood on given channel.      */
void deauth_svc_attack_broadcast(uint8_t channel);
void deauth_svc_sniff_clients(void);  /* → SNIFFING (from AP_READY)           */
void deauth_svc_sniff_skip(void);     /* → CLIENT_READY immediately           */
void deauth_svc_attack_start(void);   /* → ATTACKING (from CLIENT_READY)      */
void deauth_svc_stop(void);           /* → DONE (stops whatever is running)   */
void deauth_svc_reset(void);          /* → IDLE, clears all state             */

/* ── Per-loop tick ───────────────────────────────────────────────────────── */

/* Must be called every loop iteration regardless of state. */
void deauth_svc_tick(uint32_t now_ms);

/* ── State accessor ──────────────────────────────────────────────────────── */

deauth_svc_state_t deauth_svc_state(void);

/* ── AP list (valid when state >= AP_READY) ──────────────────────────────── */

int                      deauth_svc_ap_count(void);
const wifi_ap_entry_t*   deauth_svc_ap(int idx);
void                     deauth_svc_ap_toggle(int idx);
void                     deauth_svc_ap_select_all(void);
void                     deauth_svc_ap_select_none(void);

/* ── Client list (valid when state >= CLIENT_READY) ─────────────────────── */

int                          deauth_svc_client_count(void);
const wifi_client_entry_t*   deauth_svc_client(int idx);
void                         deauth_svc_client_toggle(int idx);
bool                         deauth_svc_broadcast_selected(void);
void                         deauth_svc_set_broadcast(bool on);

/* ── Sniff progress (valid when state == SNIFFING) ───────────────────────── */

uint8_t  deauth_svc_sniff_channel(void);
int      deauth_svc_sniff_ch_idx(void);
int      deauth_svc_sniff_ch_count(void);
uint32_t deauth_svc_sniff_elapsed_ms(void);   /* ms elapsed on current channel */

/* ── Attack metrics (valid when state == ATTACKING or DONE) ──────────────── */

uint32_t deauth_svc_frames(void);
uint32_t deauth_svc_pps(void);

/* ── Activity log ────────────────────────────────────────────────────────── */

int         deauth_svc_log_count(void);
/* i=0 oldest, i=count-1 newest */
const char* deauth_svc_log_line(int i);
/* true if new lines were added since last clear_dirty() call */
bool        deauth_svc_log_dirty(void);
void        deauth_svc_log_clear_dirty(void);

#endif /* BS_HAS_WIFI */
