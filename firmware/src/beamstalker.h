#pragma once
/*
 * beamstalker.h - top-level BeamStalker firmware API.
 *
 * Firmware version and name are injected by the build system:
 *   -DBS_VERSION=\"0.1.0\"
 *   -DBS_FW_NAME=\"BeamStalker\"
 */
#ifndef BS_VERSION
#  define BS_VERSION "0.1.0"
#endif
#ifndef BS_FW_NAME
#  define BS_FW_NAME "BeamStalker"
#endif

/*
 * SD card sub-paths — the fs HAL (bs_fs_sdcard.cpp) already roots all paths
 * under /BeamStalker/ on the card, so these are relative to that root.
 */
#define BS_PATH_SETTINGS "settings.cfg"
#define BS_PATH_LOG      "system.log"
#define BS_PATH_WIFI     "wifi"
#define BS_PATH_SSIDS    "wifi/ssids.txt"
#define BS_PATH_SNIFF    "wifi/sniff"
#define BS_PATH_EAPOL    "wifi/eapol"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise all subsystems.  Called once at startup. */
void bs_init(void);

/* Run one iteration of the main loop.  Call forever. */
void bs_run(void);

#ifdef __cplusplus
}
#endif
