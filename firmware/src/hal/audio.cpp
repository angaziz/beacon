// Audio HAL — ES8311 codec + I2S playback path.
// Compiled only when BEACON_AUDIO_SPIKE is defined; invisible to the normal beacon build.
#if !BEACON_NATIVE && defined(BEACON_AUDIO_SPIKE)

#include "audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "config/pins.h"
#include "util/log.h"

// ---------------------------------------------------------------------------
// ES8311 register map (from Espressif esp-adf / esp_codec_dev es8311 driver)
// ---------------------------------------------------------------------------
#define ES8311_REG_RESET        0x00
#define ES8311_REG_CLK1         0x01  // MCLK source + div
#define ES8311_REG_CLK2         0x02  // BCLK div
#define ES8311_REG_CLK3         0x03  // LRCK H div
#define ES8311_REG_CLK4         0x04  // LRCK L div
#define ES8311_REG_CLK5         0x05  // BCLK div control
#define ES8311_REG_CLK6         0x06  // NFS + BCLK invert
#define ES8311_REG_CLK7         0x07  // TRI-STATE
#define ES8311_REG_FMT1         0x10  // I2S format: slave, I2S Philips, 16-bit
#define ES8311_REG_FMT2         0x11  // expand
#define ES8311_REG_SYS1         0x0D  // power: enable DAC, disable ADC
#define ES8311_REG_SYS2         0x0E  // master bias
#define ES8311_REG_CHIP_CTRL1   0x00  // (same as RESET; re-used label for clarity)
#define ES8311_REG_DAC1         0x31  // DAC mute control
#define ES8311_REG_DAC2         0x32  // DAC volume (0x00 = max)
#define ES8311_REG_ANALOG1      0x44  // analog output power
#define ES8311_REG_ANALOG2      0x45  // analog output power 2
#define ES8311_REG_GPIO         0x44  // HP amp enable (same addr on ES8311)

// I2S: 16 kHz, 16-bit, mono, Philips std.
// MCLK = 256 * Fs = 256 * 16000 = 4.096 MHz (ESP32-S3 MCLK output).
// ES8311 in I2S slave mode: BCLK + LRCK driven by ESP32.
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_CHANNELS      I2S_SLOT_MODE_MONO

// DMA: 2 descriptors x 256 frames x 2 bytes = 1 KB total DMA buffer.
// The Arduino wrapper uses its own defaults (6 x 240 frames); we override via
// the channel config by calling begin() with small buffers implicitly — the
// I2SClass does not expose dma_desc_num directly, so we keep the default and
// rely on the 16 kHz low data rate (16 kHz x 2 bytes x 1 ch = 32 KB/s; the
// DMA drains fast at this rate). Stack/static PCM buf is 512 samples = 32 ms.
#define CHIME_SAMPLES      (AUDIO_SAMPLE_RATE * 3 / 10)  // 0.3 s at 16 kHz = 4800 samples

static I2SClass s_i2s;
static bool     s_audio_ok = false;
static uint32_t s_min_int_free = UINT32_MAX;

// ---------------------------------------------------------------------------
// ES8311 I2C helpers — Wire already begun by power_begin()
// ---------------------------------------------------------------------------
static bool es8311_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool es8311_probe() {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  return Wire.endTransmission() == 0;
}

// ES8311 init sequence — derived from Espressif esp-adf es8311.c and
// esp_codec_dev es8311_codec.c (commit d2a6b4, Apache 2.0).
// Target: I2S slave, Philips 16-bit, 16 kHz, DAC only, MCLK from ESP32.
//
// CAUTION: This is a best-effort sequence. The ES8311 is confirmed present
// on the Waveshare 2.16 board but this init has NOT been verified by flashing.
// If audio is silent, tune REG_ANALOG1/2 or REG_CLK1 mclk_src bit first.
static bool es8311_init_dac() {
  if (!es8311_probe()) {
    LOGE("audio: ES8311 no ACK at 0x%02X", AUDIO_ES8311_ADDR);
    return false;
  }

  // Soft reset
  es8311_write(ES8311_REG_RESET, 0x1F);
  delay(10);
  es8311_write(ES8311_REG_RESET, 0x00);

  // CLK1: MCLK from pin (not internal oscillator), no invert, no pre-div
  //   bit7=0: MCLK pin as source; bit3-0: MCLK pre-divider = /1
  es8311_write(0x01, 0x30);  // MCLK src = external pin, div = 1

  // CLK2: BCLK div — for 16 kHz, 16-bit, mono: BCLK = 2 x 16 x 16000 = 512 kHz
  //   MCLK / 8 = 512 kHz: reg 0x02 BCLKDIV = 0x00 (the ESP32 drives BCLK; codec in slave)
  es8311_write(0x02, 0x00);

  // CLK3/4: LRCK divider — slave mode, these are don't-care but zero them
  es8311_write(0x03, 0x10);
  es8311_write(0x04, 0x00);

  // CLK5: internal MCLK divider for DSP (= MCLK/1, pass-through)
  es8311_write(0x05, 0x00);

  // CLK6: no invert, no tri-state
  es8311_write(0x06, 0x03);  // bit1=1: BCLK = INPUT (slave), bit0=1: LRCK = INPUT (slave)

  // CLK7: tri-state off
  es8311_write(0x07, 0x00);

  // I2S format: slave, Philips (I2S), 16-bit word length
  //   0x10 FMT1: bit6=0 slave, bits5-4=00 I2S Philips, bits2-0=011 16-bit
  es8311_write(0x10, 0x0C);  // slave mode (bit6=0), I2S Philips, 16-bit
  es8311_write(0x11, 0x00);  // no expand

  // System: enable DAC digital, disable ADC digital
  //   0x0D SYS_DP_CTL: bit5=1 DAC powerup, bit4=0 ADC power-down
  es8311_write(0x0D, 0x01);  // power sequence enable

  // Master bias / reference
  es8311_write(0x0E, 0x02);

  // ADC power-down (we only need DAC path)
  es8311_write(0x09, 0xFF);  // ADC1 power-down
  es8311_write(0x0A, 0xFF);  // ADC2 power-down
  es8311_write(0x0B, 0xFF);  // ADC3 power-down

  // DAC power-up
  es8311_write(0x0F, 0xFF);  // enable DAC digital power

  // DAC mute off (0x31 bit6=0 = unmute)
  es8311_write(0x31, 0x00);

  // DAC volume: 0x00 = 0 dB (max), range 0x00-0xFF (0 = max, linear step)
  es8311_write(0x32, 0xBF);  // ~-0 dB (BF = recommended moderate level from esp-adf)

  // Analog output: enable HP out, enable reference
  es8311_write(0x44, 0x08);  // HPOUT L enable
  es8311_write(0x45, 0x00);  // HPOUT R enable (mono: both driven)

  LOGI("audio: ES8311 init OK");
  return true;
}

// ---------------------------------------------------------------------------
// PA control
// ---------------------------------------------------------------------------
static void pa_enable(bool on) {
  digitalWrite(AUDIO_PA_CTRL, on ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// audio_init
// ---------------------------------------------------------------------------
bool audio_init() {
  // PA starts muted
  pinMode(AUDIO_PA_CTRL, OUTPUT);
  pa_enable(false);

  // I2S: setPins(bclk, ws, dout, din=-1, mclk)
  s_i2s.setPins(AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT, -1, AUDIO_I2S_MCLK);
  if (!s_i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS)) {
    LOGE("audio: I2S begin FAIL");
    return false;
  }

  s_audio_ok = es8311_init_dac();
  if (!s_audio_ok) {
    s_i2s.end();
  }
  return s_audio_ok;
}

// ---------------------------------------------------------------------------
// audio_play_chime
// ---------------------------------------------------------------------------
// Synthesizes two tones sequentially into a static 16-bit PCM buffer and
// writes them to I2S. Total ~0.3 s, blocks on Core-0 (acceptable for spike).
void audio_play_chime(uint8_t kind) {
  if (!s_audio_ok) return;

  // Two notes: A4 (440 Hz) + C5 (523 Hz) for kind=0; C5 + E5 (659 Hz) for kind=1.
  const float freqs[2][2] = {
    { 440.0f, 523.0f },
    { 523.0f, 659.0f },
  };
  const uint8_t sel = kind & 1;

  // 512-sample static buf reused per note (1 KB on stack — safe in a task with 8 KB stack)
  static int16_t buf[512];
  const int note_samples = CHIME_SAMPLES / 2;  // split evenly between the two notes

  pa_enable(true);

  for (int note = 0; note < 2; note++) {
    float freq = freqs[sel][note];
    int remaining = note_samples;
    float phase = 0.0f;
    const float phase_inc = 2.0f * (float)M_PI * freq / (float)AUDIO_SAMPLE_RATE;

    while (remaining > 0) {
      int chunk = remaining < 512 ? remaining : 512;
      for (int i = 0; i < chunk; i++) {
        // Simple sine with a 20-sample linear fade-in/out to reduce clicks
        float env = 1.0f;
        int pos_in_note = note_samples - remaining + i;
        if (pos_in_note < 20) env = pos_in_note / 20.0f;
        if (pos_in_note > note_samples - 20) env = (note_samples - pos_in_note) / 20.0f;
        buf[i] = (int16_t)(sinf(phase) * env * 24000.0f);
        phase += phase_inc;
      }
      // write() takes bytes
      s_i2s.write((const uint8_t*)buf, (size_t)chunk * sizeof(int16_t));
      remaining -= chunk;
    }
  }

  pa_enable(false);
}

// ---------------------------------------------------------------------------
// Heap spike logging — called from loop() via BEACON_AUDIO_SPIKE gate in main
// ---------------------------------------------------------------------------
void audio_spike_log() {
  uint32_t free_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (free_int < s_min_int_free) s_min_int_free = free_int;
  // Controller greps for this prefix
  Serial.printf("HEAPSPIKE free=%u min=%u\n", free_int, s_min_int_free);
}

#endif // !BEACON_NATIVE && BEACON_AUDIO_SPIKE
