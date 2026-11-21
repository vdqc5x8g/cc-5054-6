#pragma once

#include <string>

// Surface scheduler policies. "fixed" never resizes; "elastic" grows and
// shrinks the active pool from release feedback. The rule below is the one
// selected by the trajectory experiments (see README §7): grow only when
// the expected wait for the oldest in-flight surface exceeds half a frame
// budget, otherwise wait -- growing earlier only rents memory that the
// steady state never uses, and waiting longer stalls the submit loop.
//
//   acquire: free surface -> use it.
//            none free, pool below ceiling, and the oldest in-flight
//              surface is not expected back within half a frame period
//              -> grow.
//            otherwise -> wait for the release that is due.
//   release: a surface has stayed FREE for more than 8 frame periods while
//            at least 2 surfaces are free and the pool is above its floor
//            -> retire it (shrink).
struct SchedInput {
  int active = 0;         // surfaces currently in the pool (non-retired)
  int poolMin = 2;
  int poolMax = 8;
  int freeCount = 0;
  int inFlight = 0;       // surfaces owned by the encoder right now
  double framePeriodMs = 33.3;
  double oldestWaitMs = -1;  // expected wait for the oldest in-flight release
  int64_t idleUs = 0;        // how long the most-idle FREE surface has been idle
};

enum class SchedAct { UseFree, Grow, Wait, Shrink };

struct SchedDecision {
  SchedAct act = SchedAct::Wait;
  std::string why;
};

inline SchedDecision decideAcquire(const SchedInput& in, bool elastic) {
  SchedDecision d;
  if (in.freeCount > 0) {
    d.act = SchedAct::UseFree;
    return d;
  }
  if (!elastic || in.active >= in.poolMax) {
    d.act = SchedAct::Wait;
    d.why = elastic ? "已到池上限，只能等归还" : "固定池：只能等归还";
    return d;
  }
  const double budgetMs = 0.5 * in.framePeriodMs;
  if (in.inFlight == 0) {
    d.act = SchedAct::Grow;
    d.why = "没有在途表面可等，直接增长";
    return d;
  }
  if (in.oldestWaitMs < 0 || in.oldestWaitMs > budgetMs) {
    d.act = SchedAct::Grow;
    d.why = "最旧在途表面的预计归还超过半帧预算，等待会误帧，增长";
    return d;
  }
  d.act = SchedAct::Wait;
  d.why = "归还预计在半帧预算内到达，等待比租新内存便宜";
  return d;
}

inline SchedDecision decideRelease(const SchedInput& in, bool elastic) {
  SchedDecision d;
  d.act = SchedAct::UseFree;  // "keep" sentinel: no shrink
  if (!elastic) return d;
  if (in.active <= in.poolMin) return d;
  if (in.freeCount < 2) return d;
  if (in.idleUs < (int64_t)(8 * in.framePeriodMs * 1000.0)) return d;
  d.act = SchedAct::Shrink;
  d.why = "空闲表面已连续 8 帧周期未被需要，收缩回收到下限前";
  return d;
}
