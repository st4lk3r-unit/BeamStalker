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
