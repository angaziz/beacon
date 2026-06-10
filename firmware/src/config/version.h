#pragma once

// FIRMWARE_VERSION is injected at build time by version.py (the git tag in CI, e.g.
// "v0.1.0"). This fallback keeps any build that runs without the pre-script compiling.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif
