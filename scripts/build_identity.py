"""Inject this build's identity into the firmware image.

A build that cannot say what it is cannot be released, and until now nothing supplied the
defines `src/device_info.cpp` already reads: `CAIRN_GIT_COMMIT_HEX` was never passed by any
build, so every device reported an all-zero commit, and `CAIRN_RELEASE_BUILD` was never set,
so no image ever claimed to be a release.

Three defines come from here:

  CAIRN_GIT_COMMIT_HEX  16 hex characters of HEAD, or absent when git cannot say
  CAIRN_BUILD_DIRTY     set when the tree had uncommitted changes
  CAIRN_CHANNEL         "dev", "beta" or "stable", from the CAIRN_CHANNEL environment
                        variable; scripts/release-firmware.sh sets it

and `CAIRN_RELEASE_BUILD` is set for a stable channel, which is the flag
`cairn_di_firmware_t` already carries over BLE (`CAIRN_FW_RELEASE`).

The channel is deliberately *not* part of `CAIRN_FIRMWARE_VERSION`. That string is sealed
into a bundle's 32-byte `firmware_version` beside the engine profile's id and digest, and
widening it would silently cost the engine stamp the room it needs — exactly the provenance
that firmware #39 was filed to get back.

Nothing here may fail a build. A tree without git, or a git that errors, yields no commit
define rather than an error: `device_info.cpp` already reads an absent define as "not
recorded", which is honest.
"""

import subprocess

Import("env")  # noqa: F821  (SCons injects this)

CHANNELS = ("dev", "beta", "stable")


def _git(*args):
    try:
        out = subprocess.run(
            ["git", *args],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip()


def _channel():
    raw = (env.get("ENV", {}).get("CAIRN_CHANNEL") or "").strip().lower()  # noqa: F821
    # An unrecognised or absent channel is dev and never stable: a build that cannot say
    # what it is has not earned the word that carries a promise.
    return raw if raw in CHANNELS else "dev"


flags = []

channel = _channel()
flags.append(("CAIRN_CHANNEL", '\\"%s\\"' % channel))
if channel == "stable":
    flags.append(("CAIRN_RELEASE_BUILD", None))

commit = _git("rev-parse", "HEAD")
if commit and len(commit) >= 16:
    flags.append(("CAIRN_GIT_COMMIT_HEX", '\\"%s\\"' % commit[:16]))

# An empty status means clean. A git that cannot answer is not called clean.
status = _git("status", "--porcelain")
if status is None or status != "":
    flags.append(("CAIRN_BUILD_DIRTY", None))

env.Append(CPPDEFINES=[(name, value) if value is not None else name for name, value in flags])  # noqa: F821

print(
    "cairn build identity: channel=%s commit=%s%s"
    % (
        channel,
        (commit[:12] if commit else "unknown"),
        " DIRTY" if any(n == "CAIRN_BUILD_DIRTY" for n, _ in flags) else "",
    )
)
