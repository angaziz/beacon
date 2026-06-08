#pragma once

// Signed change vs a basis value (prev-close or 24h-open). Pure + host-testable. change_pct is 0
// when basis is 0 (avoids div-by-zero; caller shows "--" if it lacks a basis).
#ifdef __cplusplus
extern "C" {
#endif
void change_compute(double value, double basis, double* change, double* change_pct);
#ifdef __cplusplus
}
#endif
