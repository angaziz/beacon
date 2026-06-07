#pragma once
// BEACON_DEV-only: seeds representative DataStore values so screens render populated,
// runs the Core-0 staleness ticker, and binds a long-press that cycles the visible
// screen's record through its plane's reachable states (Chunk-C verification harness).
void dev_seed_init(void);
