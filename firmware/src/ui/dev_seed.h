#pragma once
// BEACON_DEV-only: seeds representative DataStore values so screens render populated,
// runs the Core-0 staleness ticker, and binds a long-press that cycles the visible
// screen's record through its plane's reachable states (Chunk-C verification harness).
void dev_seed_init(void);
#if BEACON_CAPTURE
// Re-apply the seed with fresh timestamps. The screenshot sweep calls this so records read LIVE
// even when 'C' arrives long after boot (past the 180s prompt / 300s usage staleness windows).
void dev_seed_reseed(void);
#endif
