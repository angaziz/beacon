#pragma once
// Audio HAL — ES8311 codec over I2S + I2C.
// Only compiled for device builds; guarded out of native/host.
#if !BEACON_NATIVE && defined(BEACON_AUDIO_SPIKE)

#include <stdint.h>
#include <stdbool.h>

// Init I2S peripheral (master TX) + ES8311 over Wire.
// Must be called AFTER power_begin() + display_begin() (I2C bus already up).
// Returns false if ES8311 does not ACK — degrades to silent, no hang.
bool audio_init();

// Synthesize a short (~0.3 s) two-note chime and write it to I2S.
// kind selects the base pitch (0 = lower, 1 = higher).
// Unmutes PA for the duration, then mutes it. Brief blocking call on Core-0.
void audio_play_chime(uint8_t kind);

// Log internal heap free + min-ever to Serial with the HEAPSPIKE prefix.
// Intended to be called periodically from loop() or a timer.
void audio_spike_log();

#endif // !BEACON_NATIVE && BEACON_AUDIO_SPIKE
