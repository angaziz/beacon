#pragma once
// AXP2101 PMU. Rails MUST be enabled before the display (tech.md §3);
// ALDO2 = DSI_PWR_EN is the rail the stock Waveshare demo omits.
bool power_begin();   // true if the AXP2101 answered on I2C and rails were set
