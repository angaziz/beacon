#pragma once

// IANA timezone id -> POSIX TZ string (for setenv("TZ", ...)). Arduino-free so it is
// host-unit-testable. A full tz database is out of scope (tech.md); only the supported set is
// carried. Returns NULL for an unknown id (caller falls back to UTC + logs).
#ifdef __cplusplus
extern "C" {
#endif
const char* tz_iana_to_posix(const char* iana);
#ifdef __cplusplus
}
#endif
