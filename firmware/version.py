Import("env")
import os

# FIRMWARE_VERSION is set by CI (release.yml) to the git tag, e.g. "v0.1.0".
# Local builds have no env var => "dev".
v = os.environ.get("FIRMWARE_VERSION", "") or "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(v))])
