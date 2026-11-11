"""Temperature-control quality metrics for a batch.

The batch is split wherever the target changes. Each piece has a warm-up
(or cool-down) phase followed by a hold phase:

    start ──approach──▶ reach ──settle (60 min)──▶ hold ……… end
                    (first time within 0.1 °C of target)

The hold phases of all pieces are pooled to compute:

  mean_offset_c   time-weighted mean of (temp − target): steady bias
  band90_c        [5th, 95th] percentile of (temp − target): typical spread
  p2p_c           max − min of (temp − target) during hold
  within_05_pct   % of hold time within ±0.5 °C of target
  overshoot_c     largest excursion past the target while settling, after
                  approaching it (heating up: temp above target; cooling
                  down: below). None if every piece started at the target.
  cycles_per_h    heater on-switches per hour of hold (plug wear)
  heat_duty_pct   % of hold time with the heater on

Gaps in telemetry (device offline) don't count as time: each reading
covers at most MAX_GAP_S until the next one.
"""

from __future__ import annotations

import bisect
from typing import Any, Iterable, Sequence

REACH_TOL_C = 0.1
AT_TARGET_C = 0.25        # a piece that starts this close is already "reached"
SETTLE_S = 3600
MAX_GAP_S = 120
WITHIN_C = 0.5


def _percentile(sorted_vals: Sequence[float], q: float) -> float:
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def _target_at(changes: Sequence[tuple[float, float]], t: float) -> float | None:
    """Target in force at time t, from (ts, setpoint) changes sorted by ts."""
    i = bisect.bisect_right([c[0] for c in changes], t) - 1
    return changes[i][1] if i >= 0 else None


def compute(
    rows: Iterable[tuple[float, float | None, int | None]],
    targets: Sequence[tuple[float, float]],
    t_start: float,
    t_end: float,
) -> dict[str, Any]:
    """rows: (unix_ts, temp_c or None, relay1); targets: (unix_ts, setpoint)
    changes sorted by time (an entry at or before t_start gives the initial
    target). Returns a JSON-serializable dict."""
    data = [(t, T, 1 if r else 0) for t, T, r in rows if t_start <= t <= t_end]
    data.sort(key=lambda x: x[0])
    result: dict[str, Any] = {
        "t_start": t_start, "t_end": t_end, "settle_min": SETTLE_S // 60,
        "pieces": [], "hold_hours": 0.0,
        "mean_offset_c": None, "band90_c": None, "p2p_c": None,
        "within_05_pct": None, "overshoot_c": None,
        "cycles_per_h": None, "heat_duty_pct": None,
    }
    if not data or not targets:
        result["note"] = "no telemetry" if not data else "no target recorded"
        return result

    # Piece boundaries: batch start plus every target change inside it.
    bounds = [t_start] + [ts for ts, _ in targets if t_start < ts < t_end] + [t_end]

    hold_err: list[tuple[float, float]] = []      # (error, weight seconds)
    hold_relay: list[tuple[int, float]] = []
    cycles = 0
    overshoots: list[float] = []

    for a, b in zip(bounds, bounds[1:]):
        sp = _target_at(targets, a)
        seg = [d for d in data if a <= d[0] < b or (b == t_end and d[0] == b)]
        temps = [d for d in seg if d[1] is not None]
        if sp is None or not temps:
            continue
        first_err = temps[0][1] - sp
        direction = 0 if abs(first_err) <= AT_TARGET_C else (1 if first_err < 0 else -1)
        # 1 = heating up to target, -1 = cooling down to it, 0 = already there
        reach = a
        if direction != 0:
            reach = None
            for t, T, _ in temps:
                if direction * (sp - T) <= REACH_TOL_C:
                    reach = t
                    break
        piece: dict[str, Any] = {"from": a, "to": b, "target": sp,
                                 "approach": {1: "heat", -1: "cool", 0: "at target"}[direction],
                                 "reached_at": reach, "hold_from": None}
        result["pieces"].append(piece)
        if reach is None:
            continue
        hold_from = reach + SETTLE_S
        if direction != 0:
            past = [direction * (T - sp) for t, T, _ in temps if reach <= t < hold_from]
            if past:
                overshoots.append(max(0.0, max(past)))
        if hold_from >= b:
            continue
        piece["hold_from"] = hold_from

        prev_relay = None
        for i, (t, T, r) in enumerate(seg):
            if t < hold_from:
                prev_relay = r
                continue
            nxt = seg[i + 1][0] if i + 1 < len(seg) else min(b, t + 30)
            w = max(0.0, min(nxt - t, MAX_GAP_S))
            hold_relay.append((r, w))
            if prev_relay == 0 and r == 1:
                cycles += 1
            prev_relay = r
            if T is not None:
                hold_err.append((T - sp, w))

    if overshoots:
        result["overshoot_c"] = round(max(overshoots), 2)

    total_w = sum(w for _, w in hold_err)
    relay_w = sum(w for _, w in hold_relay)
    if total_w <= 0:
        result["note"] = "no hold period yet (still settling or batch too short)"
        return result

    errs = sorted(e for e, _ in hold_err)
    result["hold_hours"] = round(relay_w / 3600, 2)
    result["mean_offset_c"] = round(sum(e * w for e, w in hold_err) / total_w, 3)
    result["band90_c"] = [round(_percentile(errs, 0.05), 2), round(_percentile(errs, 0.95), 2)]
    result["p2p_c"] = round(errs[-1] - errs[0], 2)
    result["within_05_pct"] = round(
        100 * sum(w for e, w in hold_err if abs(e) <= WITHIN_C + 1e-9) / total_w, 1)
    if relay_w > 0:
        result["cycles_per_h"] = round(cycles / (relay_w / 3600), 1)
        result["heat_duty_pct"] = round(100 * sum(r * w for r, w in hold_relay) / relay_w, 1)
    return result
