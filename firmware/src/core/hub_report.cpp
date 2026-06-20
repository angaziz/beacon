#include "core/hub_report.h"
#include "core/hub_proto.h"   // hub_report_plan / hub_build_report_frame / HUB_FRAME_MAX

int hub_emit_report(HubLink* link, const ticker_runtime_t* rows, int count) {
  if (!link || !rows) return 0;
  if (count > MAX_TICKERS) count = MAX_TICKERS;
  int gs[MAX_TICKERS];
  int parts = hub_report_plan(rows, count, gs);
  if (parts < 1) return 0;                              // nothing emittable (e.g. unmappable enum)
  char buf[HUB_FRAME_MAX];
  for (int p = 0; p < parts; p++) {
    int lo = gs[p];
    int hi = (p + 1 < parts) ? gs[p + 1] : count;
    size_t n = hub_build_report_frame(rows, lo, hi, p, parts, buf, sizeof(buf));
    if (!n) return -1;                                  // serialize failed => retry whole report later
    link->flush();                                      // push the prior chunk out so s_out has room (#106)
    if (!link->send(buf, n)) return -1;                 // enqueue failed => retry whole report later
  }
  link->flush();                                        // push the final chunk out promptly
  return parts;
}
