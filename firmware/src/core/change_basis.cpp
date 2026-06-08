#include "core/change_basis.h"

void change_compute(double value, double basis, double* change, double* change_pct) {
  double c = value - basis;
  if (change)     *change = c;
  if (change_pct) *change_pct = (basis != 0.0) ? (c / basis) * 100.0 : 0.0;
}
