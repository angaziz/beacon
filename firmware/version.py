Import("env")
import os

# FIRMWARE_VERSION is set by CI (release-firmware.yml) to the git tag, e.g.
# "firmware-v0.1.0". Strip the "firmware-" prefix so the device shows "v0.1.0".
# Local builds have no env var => "dev".
v = os.environ.get("FIRMWARE_VERSION", "")
if v.startswith("firmware-"):
    v = v[len("firmware-"):]
v = v or "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(v))])
