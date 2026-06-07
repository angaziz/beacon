#pragma once
// AXP2101 PMU. Rails MUST be enabled before the display (tech.md §3);
// ALDO2 = DSI_PWR_EN is the rail the stock Waveshare demo omits.
bool power_begin();   // true if the AXP2101 answered on I2C and rails were set
int  power_battery_pct();   // 0..100, or -1 if no battery / unknown
bool power_charging();      // true if charging from USB
