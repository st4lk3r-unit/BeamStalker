"""
patch_libnet80211.py — Pre-build: weaken ieee80211_raw_frame_sanity_check.

Background
----------
Espressif's closed-source libnet80211.a contains a strong symbol
``ieee80211_raw_frame_sanity_check`` that blocks some raw 802.11 management
frames passed through esp_wifi_80211_tx().

BeamStalker intentionally provides a project-local override in
arch/esp32/arduino/wsl_bypasser.c.  For that override to win cleanly, this
script weakens the copy of ieee80211_raw_frame_sanity_check inside the vendor
libnet80211.a before link time.

Important
---------
Do NOT make BeamStalker's override weak: then the vendor SDK's strong symbol
wins and the bypass is disabled.  The vendor symbol must be weakened; the
project override must stay strong.

Idempotency
-----------
A sentinel file <lib>.wsl_patched is created next to libnet80211.a after a
successful patch.  Subsequent builds skip the objcopy call.
"""
Import("env")  # noqa: F821  (PlatformIO SCons injection)

import os
import shutil
import subprocess

# ── Config ────────────────────────────────────────────────────────────────

SYMBOL = "ieee80211_raw_frame_sanity_check"

# chip id → objcopy binary name
# Xtensa parts use chip-specific objcopy names.  RISC-V ESP chips use the
# shared riscv32-esp toolchain package.
OBJCOPY_MAP = {
    "esp32":   "xtensa-esp32-elf-objcopy",
    "esp32s2": "xtensa-esp32s2-elf-objcopy",
    "esp32s3": "xtensa-esp32s3-elf-objcopy",
    "esp32c2": "riscv32-esp-elf-objcopy",
    "esp32c3": "riscv32-esp-elf-objcopy",
    "esp32c5": "riscv32-esp-elf-objcopy",
    "esp32c6": "riscv32-esp-elf-objcopy",
    "esp32h2": "riscv32-esp-elf-objcopy",
    "esp32p4": "riscv32-esp-elf-objcopy",
}

# ── Helpers ───────────────────────────────────────────────────────────────

def _pio_home():
    return os.environ.get("PLATFORMIO_HOME_DIR",
                          os.path.join(os.path.expanduser("~"), ".platformio"))


def _packages_dir():
    # $PROJECT_PACKAGES_DIR is available in recent PlatformIO versions and
    # covers custom PLATFORMIO_HOME_DIR values.  Fall back to ~/.platformio.
    project_packages = env.subst("$PROJECT_PACKAGES_DIR")  # noqa: F821
    if project_packages and not project_packages.startswith("$"):
        return project_packages
    return os.path.join(_pio_home(), "packages")


def _find_objcopy(name):
    """Search PlatformIO toolchain packages, then fall back to PATH."""
    packages = _packages_dir()
    if os.path.isdir(packages):
        for pkg in os.listdir(packages):
            candidate = os.path.join(packages, pkg, "bin", name)
            if os.path.isfile(candidate):
                return candidate
    return shutil.which(name)


def _candidate_libs(mcu):
    """Return possible libnet80211.a locations for Arduino-ESP32 2.x/3.x.

    Arduino-ESP32 2.x stores SDK libs below framework-arduinoespressif32/tools.
    Arduino-ESP32 3.x / pioarduino stores chip libs in the separate
    framework-arduinoespressif32-libs package.
    """
    packages = _packages_dir()
    framework_dir = env.subst("$FRAMEWORK_DIR")  # noqa: F821
    candidates = []

    def add(path):
        if path and path not in candidates:
            candidates.append(path)

    add(os.path.join(framework_dir, "tools", "sdk", mcu, "lib", "libnet80211.a"))
    add(os.path.join(packages, "framework-arduinoespressif32", "tools", "sdk", mcu, "lib", "libnet80211.a"))
    add(os.path.join(packages, "framework-arduinoespressif32-libs", mcu, "lib", "libnet80211.a"))

    return candidates


def _patch_archive(lib_path, objcopy):
    sentinel = lib_path + ".wsl_patched"
    if os.path.isfile(sentinel):
        print(f"[wsl_bypass] {os.path.basename(lib_path)} already patched — ok")
        return True

    tmp = lib_path + ".wsl_tmp"
    try:
        subprocess.check_call([
            objcopy,
            f"--weaken-symbol={SYMBOL}",
            lib_path,
            tmp,
        ])
        shutil.move(tmp, lib_path)
        open(sentinel, "w").close()
        print(f"[wsl_bypass] Patched {lib_path}: {SYMBOL} is now weak")
        return True
    except subprocess.CalledProcessError as exc:
        print(f"[wsl_bypass] objcopy failed for {lib_path}: {exc}")
        if os.path.isfile(tmp):
            os.remove(tmp)
        return False

# ── Main ──────────────────────────────────────────────────────────────────

mcu = env.subst("$BOARD_MCU").lower()  # noqa: F821  e.g. esp32s3/esp32c6

if not mcu or mcu.startswith("$"):
    # native / non-board builds
    pass
elif mcu not in OBJCOPY_MAP:
    print(f"[wsl_bypass] MCU '{mcu}' not in OBJCOPY_MAP — skipping")
else:
    objcopy_name = OBJCOPY_MAP[mcu]
    objcopy = _find_objcopy(objcopy_name)
    if not objcopy:
        print(f"[wsl_bypass] {objcopy_name} not found — cannot patch")
    else:
        patched = False
        searched = []
        for lib_path in _candidate_libs(mcu):
            searched.append(lib_path)
            if os.path.isfile(lib_path):
                patched = _patch_archive(lib_path, objcopy)
                break

        if not patched:
            print("[wsl_bypass] libnet80211.a not found; searched:")
            for path in searched:
                print(f"  {path}")
