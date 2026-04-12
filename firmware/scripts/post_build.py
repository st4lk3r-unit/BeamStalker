"""
post_build.py — BeamStalker post-build artifact copy.

Copies the output binary to:
  bin/BeamStalker-<version>-<device>.bin

<version> : value of BS_VERSION build flag (e.g. 0.1.0)
<device>  : last segment of PIO env name  (esp32s3-tpager → tpager)

Trigger timing:
  native   — fires on the ELF/program (final output)
  ESP32    — fires on firmware.bin (created after ELF by esptool)
"""
Import("env")  # noqa: F821  (PlatformIO SCons injection)

import os
import re
import shutil


def _extract_version(build_flags):
    for flag in build_flags:
        m = re.search(r'BS_VERSION=\\"([^\\"]+)\\"', str(flag))
        if m:
            return m.group(1)
    return "0.0.0"


def copy_artifact(source, target, env):  # noqa: ARG001
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    pioenv      = env.subst("$PIOENV")
    platform    = env.subst("$PIOPLATFORM")

    version = _extract_version(env.get("BUILD_FLAGS", []))
    device  = pioenv

    bin_dir = os.path.join(project_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)

    if platform == "native":
        src = os.path.join(build_dir, "program")
    else:
        src = os.path.join(build_dir, "firmware.bin")

    dst = os.path.join(bin_dir, f"BeamStalker-{version}-{device}.bin")

    if not os.path.isfile(src):
        print(f"\n[BeamStalker] WARNING: artifact not found at {src}")
        return

    shutil.copy2(src, dst)
    rel = os.path.relpath(dst, project_dir)
    size_kb = os.path.getsize(dst) / 1024
    print(f"\n  ▓ BeamStalker artifact  →  {rel}  ({size_kb:.1f} KB)\n")


platform = env.subst("$PIOPLATFORM")  # noqa: F821

if platform == "native":
    env.AddPostAction("${BUILD_DIR}/${PROGNAME}${PROGSUFFIX}", copy_artifact)  # noqa: F821
else:
    # firmware.bin is generated after ELF by esptool; hook there
    env.AddPostAction("$BUILD_DIR/firmware.bin", copy_artifact)  # noqa: F821
