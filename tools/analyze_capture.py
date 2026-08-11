#!/usr/bin/env python3
"""
Analyse an OOK capture: recover pulse timings, segment frames, and identify the
line coding.

The pipeline, in order:

  raw IQ  ->  amplitude envelope  ->  on/off threshold  ->  run lengths (us)
          ->  run-length clusters ->  base tick         ->  run lengths (ticks)
          ->  frame segmentation  ->  encoding tests    ->  signal.h snippet

The single most important output is the RUN-LENGTH CLUSTER table. If it shows
clusters at 3x and 4x the base tick, then a capture that only contains 1x and 2x
runs was clamped by a two-bucket "short or long" classifier, and is missing
symbols that no amount of timing calibration can recover.

Usage:
    # From a raw rtl_sdr capture (uint8 interleaved I/Q)
    ./analyze_capture.py --iq original.iq --sample-rate 2048000

    # From a URH .complex16s capture (int16 interleaved I/Q)
    ./analyze_capture.py --iq press.complex16s --iq-format s16 --sample-rate 2000000

    # From microsecond run lengths (one per line, or comma separated).
    # Signed: positive = RF ON, negative = RF OFF.
    ./analyze_capture.py --us timings.txt

    # From an existing signal.h (SIGNAL_BEEP_TICKS or legacy SIGNAL_BEEP)
    ./analyze_capture.py --header ../include/signal.h

Requires: numpy (only for --iq mode).
"""

import argparse
import re
import sys
from collections import Counter

# ---------------------------------------------------------------------------
# Input loading
# ---------------------------------------------------------------------------


def load_iq(path, sample_rate, smooth_us, threshold_frac, min_run_us, iq_format="u8"):
    """Raw IQ -> signed microsecond run lengths."""
    try:
        import numpy as np
    except ImportError:
        sys.exit("ERROR: --iq mode needs numpy.  pip3 install numpy")

    # u8  : rtl_sdr native, unsigned 8-bit interleaved I,Q centred on 127.5
    # s16 : URH .complex16s, signed 16-bit interleaved I,Q centred on 0
    # f32 : signed 32-bit float interleaved I,Q centred on 0
    dtype, offset = {
        "u8": (np.uint8, 127.5),
        "s16": (np.int16, 0.0),
        "f32": (np.float32, 0.0),
    }[iq_format]

    raw = np.fromfile(path, dtype=dtype)
    if raw.size < 4:
        sys.exit(f"ERROR: {path} is too small to be an IQ capture")
    if raw.size % 2:
        raw = raw[:-1]

    iq = raw.astype(np.float32) - offset
    env = np.hypot(iq[0::2], iq[1::2])

    duration_s = env.size / sample_rate
    print(f"  file            : {path}")
    print(f"  samples         : {env.size:,}  ({duration_s:.3f} s @ {sample_rate/1e6:.3f} MSPS)")

    # Smooth the envelope so single-sample noise cannot create fake edges.
    win = max(1, int(smooth_us * 1e-6 * sample_rate))
    if win > 1:
        kernel = np.ones(win, dtype=np.float32) / win
        env = np.convolve(env, kernel, mode="same")
    print(f"  smoothing       : {win} samples ({smooth_us} us)")

    if threshold_frac is None:
        thr = otsu_threshold(env)
        how = "Otsu (automatic)"
    else:
        thr = env.max() * threshold_frac
        how = f"{threshold_frac:.2f} x peak"

    noise, peak = float(np.percentile(env, 10)), float(np.percentile(env, 99.9))
    print(f"  noise floor     : {noise:.1f}")
    print(f"  signal peak     : {peak:.1f}")
    print(f"  threshold       : {thr:.1f}   [{how}]")
    if peak <= noise * 1.5:
        print("  !! peak is barely above the noise floor - capture may be too weak")

    on = env > thr
    runs = runs_from_binary(on, sample_rate)

    # Drop sub-minimum runs: these are threshold chatter, not real symbols.
    keep = [r for r in runs if abs(r) >= min_run_us]
    if len(keep) != len(runs):
        print(f"  discarded       : {len(runs) - len(keep)} runs shorter than {min_run_us} us (noise)")
    return trim_edges(keep)


def otsu_threshold(env, nbins=256):
    """Split a bimodal histogram at the point of maximum between-class variance."""
    import numpy as np

    hist, edges = np.histogram(env, bins=nbins)
    centres = (edges[:-1] + edges[1:]) / 2.0
    total = hist.sum()
    if total == 0:
        return float(env.max()) / 2.0

    w0 = np.cumsum(hist).astype(np.float64)
    w1 = total - w0
    csum = np.cumsum(hist * centres).astype(np.float64)
    grand = csum[-1]

    with np.errstate(invalid="ignore", divide="ignore"):
        m0 = csum / w0
        m1 = (grand - csum) / w1
        between = w0 * w1 * (m0 - m1) ** 2
    between[~np.isfinite(between)] = 0.0
    return float(centres[int(np.argmax(between))])


def runs_from_binary(on, sample_rate):
    """Binary on/off array -> signed microsecond run lengths."""
    import numpy as np

    edges = np.flatnonzero(np.diff(on.astype(np.int8))) + 1
    bounds = np.concatenate(([0], edges, [on.size]))
    lengths = np.diff(bounds)

    out = []
    for start, n in zip(bounds[:-1], lengths):
        us = n * 1e6 / sample_rate
        out.append(us if on[start] else -us)
    return out


def trim_edges(runs):
    """Drop leading/trailing silence, which is capture padding, not signal."""
    while runs and runs[0] < 0:
        runs.pop(0)
    while runs and runs[-1] < 0:
        runs.pop()
    return runs


def load_us(path):
    text = open(path).read()
    vals = [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", text)]
    if not vals:
        sys.exit(f"ERROR: no numbers found in {path}")
    return vals


def load_header(path):
    """Read SIGNAL_BEEP_TICKS (ticks) or legacy SIGNAL_BEEP (microseconds)."""
    src = open(path).read()
    base = 1.0
    m = re.search(r"#define\s+BASE_TICK_US\s+(\d+)", src)
    if m:
        base = float(m.group(1))

    for macro in ("SIGNAL_BEEP_TICKS", "SIGNAL_BEEP"):
        idx = src.find("#define " + macro)
        if idx == -1:
            continue
        vals = [int(x) for x in re.findall(r"-?\d+", src[idx:])]
        scale = base if macro == "SIGNAL_BEEP_TICKS" else 1.0
        if macro == "SIGNAL_BEEP":
            base = 1.0
        print(f"  read {macro} from {path}  ({len(vals)} elements, base tick {base:g} us)")
        return [v * scale for v in vals]

    sys.exit(f"ERROR: no SIGNAL_BEEP_TICKS or SIGNAL_BEEP macro in {path}")


# ---------------------------------------------------------------------------
# Clustering and base-tick recovery
# ---------------------------------------------------------------------------


def cluster_1d(values, rel_tol=0.25):
    """Group sorted values, splitting wherever the gap exceeds rel_tol of the mean."""
    vals = sorted(values)
    clusters = [[vals[0]]]
    for v in vals[1:]:
        cur = clusters[-1]
        centre = sum(cur) / len(cur)
        if v - cur[-1] <= rel_tol * centre:
            cur.append(v)
        else:
            clusters.append([v])
    return clusters


def estimate_base(runs):
    """Recover the symbol period.

    A tight first pass isolates the shortest coherent group of runs; that mean is
    a rough base tick. It is then refined by least squares over every run that
    quantises cleanly, so the estimate uses the whole capture rather than only
    the shortest pulses.
    """
    mags = sorted(abs(r) for r in runs)
    first = [mags[0]]
    for v in mags[1:]:
        if v - first[-1] <= 0.15 * (sum(first) / len(first)):
            first.append(v)
        else:
            break
    base = sum(first) / len(first)

    for _ in range(4):
        fit = [(m, round(m / base)) for m in mags]
        fit = [(m, k) for m, k in fit if k >= 1 and abs(m / base - k) < 0.3]
        if not fit:
            break
        new = sum(m for m, _ in fit) / sum(k for _, k in fit)
        if abs(new - base) < 1e-6:
            break
        base = new
    return base


def report_clusters(runs):
    """Group runs by their integer tick count. Returns (base tick, longest run)."""
    base = estimate_base(runs)

    groups = {}
    for r in runs:
        k = max(1, round(abs(r) / base))
        groups.setdefault(k, []).append(abs(r))

    print(f"\n  {'x base':>7}  {'mean (us)':>11}  {'count':>6}  {'spread':>8}  {'error':>7}")
    print("  " + "-" * 48)
    worst = 0.0
    for k in sorted(groups):
        g = groups[k]
        centre = sum(g) / len(g)
        spread = max(g) - min(g)
        err = abs(centre / base - k) / k * 100
        worst = max(worst, err)
        print(f"  {k:>7}  {centre:>11.1f}  {len(g):>6}  {spread:>8.1f}  {err:>6.1f}%")

    print(f"\n  estimated base tick : {base:.1f} us")
    if worst > 8:
        print(f"  !! worst group deviates {worst:.1f}% from an exact multiple.")
        print("     The base tick may be wrong, or the capture is noisy.")

    longest = max(groups)
    print(f"  longest run         : {longest}x base tick")
    if longest <= 2:
        print("  (runs capped at 2x - only meaningful once the encoding is known,")
        print("   since PWM and Manchester cap at 2x by design. See the verdict below.)")
    return base, longest


# ---------------------------------------------------------------------------
# Frame segmentation
# ---------------------------------------------------------------------------


def segment_frames(ticks, gap_ticks):
    """Split on long RF-OFF runs. Returns (frames, gap lengths in ticks)."""
    frames, gaps, cur = [], [], []
    for t in ticks:
        if t < 0 and abs(t) >= gap_ticks:
            if cur:
                frames.append(cur)
                cur = []
            gaps.append(abs(t))
        else:
            cur.append(t)
    if cur:
        frames.append(cur)
    return frames, gaps


def auto_gap_ticks(ticks):
    """Find the inter-frame gap threshold, or None if there is no clear break.

    An inter-frame gap is far longer than any run inside a frame, so it shows up
    as a large multiplicative jump in the sorted RF-OFF run lengths. Requiring a
    real jump stops a legitimately long NRZ run from being mistaken for a frame
    boundary, which would invent phantom frames and fake a rolling code.
    """
    lows = sorted({abs(t) for t in ticks if t < 0})
    if len(lows) < 2:
        return None
    best_ratio, best_thr = 1.0, None
    for a, b in zip(lows, lows[1:]):
        if a > 0 and b / a > best_ratio:
            best_ratio, best_thr = b / a, (a + b) / 2.0
    return best_thr if best_ratio >= 2.5 else None


def report_frames(ticks, base, gap_ticks):
    if gap_ticks is None:
        print("  inter-frame gap threshold : none detected - treating capture as a single frame")
        print("  frames found              : 1")
        print("  (no RF-OFF run stands out as a frame break. If you captured a long")
        print("   button hold and expected several frames, the gap may be too short")
        print("   to distinguish - force it with --gap-ticks.)")
        return [ticks]

    frames, gaps = segment_frames(ticks, gap_ticks)
    print(f"  inter-frame gap threshold : {gap_ticks:.1f}x base tick ({gap_ticks * base:.0f} us)")
    print(f"  frames found              : {len(frames)}")

    if gaps:
        print(f"  gap lengths (ticks)       : min {min(gaps)}  max {max(gaps)}  mean {sum(gaps)/len(gaps):.1f}")
        print(f"  gap lengths (us)          : mean {sum(gaps)/len(gaps)*base:.0f}")

    if len(frames) <= 1:
        return frames

    sizes = Counter(len(f) for f in frames)
    print(f"  frame lengths (elements)  : {dict(sizes)}")

    # Compare only full-length frames. A capture normally starts or ends
    # mid-frame, and those fragments are not evidence of a varying payload.
    modal = sizes.most_common(1)[0][0]
    full = [f for f in frames if len(f) == modal]
    fragments = len(frames) - len(full)
    if fragments:
        print(f"  partial frames ignored    : {fragments} (capture edges)")

    if len(full) < 2:
        print("  only one complete frame - capture a longer hold to compare repeats")
        return full or frames

    identical = all(f == full[0] for f in full)
    print(f"  complete frames compared  : {len(full)}")
    print(f"  all identical             : {'YES - fixed code' if identical else 'NO'}")
    if not identical:
        diffs = sum(1 for f in full[1:] for a, b in zip(full[0], f) if a != b)
        print(f"  differing elements        : {diffs}")
        print("  -> the payload changes between repeats. Check for a rolling code,")
        print("     a counter, or simply a noisy capture (re-run closer to the remote).")
    return full


# ---------------------------------------------------------------------------
# Encoding tests
# ---------------------------------------------------------------------------


def expand_levels(ticks):
    levels = []
    for t in ticks:
        levels += [1 if t > 0 else 0] * int(round(abs(t)))
    return levels


def test_pwm(ticks):
    """PWM: constant symbol PERIOD, exactly two symbol types."""
    best = None
    for off in (0, 1):
        pairs = [(ticks[i], ticks[i + 1]) for i in range(off, len(ticks) - 1, 2)]
        if not pairs:
            continue
        periods = Counter(abs(a) + abs(b) for a, b in pairs)
        types = Counter(pairs)
        ok = len(periods) == 1 and len(types) == 2
        detail = f"offset {off}: {len(periods)} period(s) {sorted(periods)}, {len(types)} symbol type(s)"
        if ok:
            return True, detail
        best = best or detail
    return False, best or "no pairs"


def test_ppm(ticks):
    """PPM: constant pulse width, information carried in the gaps."""
    hi = set(int(t) for t in ticks if t > 0)
    lo = set(int(-t) for t in ticks if t < 0)
    if len(hi) == 1 and len(lo) > 1:
        return True, f"constant HIGH {hi.pop()}, gaps vary {sorted(lo)}"
    if len(lo) == 1 and len(hi) > 1:
        return True, f"inverse PPM: constant LOW {lo.pop()}, pulses vary {sorted(hi)}"
    return False, f"HIGH varies {sorted(hi)} and LOW varies {sorted(lo)} - neither is constant"


def test_manchester(ticks):
    """Manchester: every cell has a mid-cell transition, so never HH or LL."""
    levels = expand_levels(ticks)
    best = None
    for off in (0, 1):
        seq = levels[off:]
        n = len(seq) // 2
        if n < 4:
            continue
        illegal = sum(1 for i in range(n) if seq[2 * i] == seq[2 * i + 1])
        pct = illegal / n * 100
        detail = f"offset {off}: {illegal}/{n} illegal cells ({pct:.1f}%)"
        if illegal == 0:
            return True, detail
        if best is None or pct < best[0]:
            best = (pct, detail)
    return False, best[1] if best else "too short"


def test_biphase(ticks):
    """Biphase (FM0/FM1): a level flip at every cell boundary."""
    levels = expand_levels(ticks)
    best = None
    for off in (0, 1):
        seq = levels[off:]
        n = len(seq) // 2
        if n < 4:
            continue
        viol = sum(1 for i in range(1, n) if seq[2 * i - 1] == seq[2 * i])
        pct = viol / max(1, n - 1) * 100
        detail = f"offset {off}: {viol}/{n-1} boundary violations ({pct:.1f}%)"
        if viol == 0:
            return True, detail
        if best is None or pct < best[0]:
            best = (pct, detail)
    return False, best[1] if best else "too short"


def test_nrz(ticks):
    """NRZ accepts any run length - but a hard cap at 2 is itself suspicious."""
    runs = [int(round(abs(t))) for t in ticks]
    dist = Counter(runs)
    longest = max(runs)
    if longest > 2:
        return True, f"runs up to {longest}x present, distribution {dict(sorted(dist.items()))}"
    # Under a random model each run has P(len>=3) = 0.25.
    expected = len(runs) * 0.25
    return False, (
        f"runs capped at 2x. Random data would give ~{expected:.0f} runs of 3+; "
        f"there are none. Distribution {dict(sorted(dist.items()))}"
    )


ENCODINGS = [
    ("PWM", "constant symbol period, 2 symbol types", test_pwm),
    ("PPM", "constant pulse width, gaps carry data", test_ppm),
    ("Manchester", "mid-cell transition always, no HH/LL", test_manchester),
    ("Biphase FM0/FM1", "level flip at every cell boundary", test_biphase),
    ("NRZ", "level is the bit, runs vary freely", test_nrz),
]


def report_encodings(ticks):
    passed = []
    for name, fingerprint, fn in ENCODINGS:
        ok, detail = fn(ticks)
        print(f"  {'PASS' if ok else 'fail':>4}  {name:<16} {fingerprint}")
        print(f"        {detail}")
        if ok:
            passed.append(name)

    return passed


def final_verdict(passed, longest):
    """Combine the encoding result with the run-length cap into one diagnosis."""
    section("5. VERDICT")
    if len(passed) == 1:
        print(f"  The encoding is {passed[0]}.")
        if longest <= 2:
            print(f"  Runs cap at 2x, which {passed[0]} does by design - not a problem.")
        return
    if passed:
        print(f"  Consistent with more than one encoding: {', '.join(passed)}.")
        print("  Capture more frames to disambiguate.")
        return

    print("  No standard encoding matches this capture.")
    if longest <= 2:
        print()
        print("  *** The capture is very likely CLAMPED. ***")
        print("  Runs cap at 2x, but no encoding that caps at 2x (PWM, Manchester,")
        print("  biphase) actually fits. The combination points at a two-bucket")
        print("  short/long classifier having collapsed genuine 3x and 4x runs into")
        print("  the 2x bucket - which destroys symbols that no amount of timing")
        print("  calibration can recover.")
        print()
        print("  Re-capture with the pulse classifier disabled, or measure the run")
        print("  lengths directly from the envelope, and re-run this script.")
    else:
        print(f"  Runs reach {longest}x, so the capture is not clamped.")
        print("  This may be a non-standard or vendor-specific line code.")


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def emit_header(ticks, base, per_line=24):
    print(f"\n#define BASE_TICK_US {int(round(base))}")
    print("\n// clang-format off")
    print("#define SIGNAL_BEEP_TICKS { \\")
    ints = [int(round(t)) for t in ticks]
    for i in range(0, len(ints), per_line):
        chunk = ", ".join(f"{v:2d}" for v in ints[i : i + per_line])
        tail = "," if i + per_line < len(ints) else ""
        print(f"  {chunk}{tail} \\")
    print("}")
    print("// clang-format on")


def section(title):
    print(f"\n{'=' * 62}\n{title}\n{'=' * 62}")


def main():
    p = argparse.ArgumentParser(
        description="Recover pulse timings and identify the line coding of an OOK capture.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--iq", metavar="FILE", help="raw IQ capture (interleaved I/Q, see --iq-format)")
    src.add_argument("--us", metavar="FILE", help="signed microsecond run lengths")
    src.add_argument("--header", metavar="FILE", help="an existing signal.h")

    p.add_argument("--sample-rate", type=float, default=2048000, help="IQ sample rate (default 2048000)")
    p.add_argument("--iq-format", choices=["u8", "s16", "f32"], default="u8",
                   help="IQ sample format: u8 = rtl_sdr, s16 = URH .complex16s, f32 (default u8)")
    p.add_argument("--smooth-us", type=float, default=20, help="envelope smoothing window (default 20)")
    p.add_argument("--threshold", type=float, default=None, help="threshold as fraction of peak (default: Otsu)")
    p.add_argument("--min-run-us", type=float, default=50, help="discard runs shorter than this (default 50)")
    p.add_argument("--base-tick", type=float, default=None, help="force the base tick instead of detecting it")
    p.add_argument("--gap-ticks", type=float, default=None,
                   help="frame split threshold in base ticks (default: auto-detect)")
    p.add_argument("--emit-header", action="store_true", help="print a signal.h snippet for the first frame")
    args = p.parse_args()

    section("1. INPUT")
    if args.iq:
        runs_us = load_iq(args.iq, args.sample_rate, args.smooth_us, args.threshold, args.min_run_us,
                          args.iq_format)
    elif args.us:
        runs_us = load_us(args.us)
    else:
        runs_us = load_header(args.header)

    if len(runs_us) < 8:
        sys.exit(f"ERROR: only {len(runs_us)} runs recovered - nothing to analyse")

    total = sum(abs(r) for r in runs_us)
    print(f"  runs recovered  : {len(runs_us)}")
    print(f"  total duration  : {total:.0f} us ({total/1000:.1f} ms)")

    section("2. RUN-LENGTH CLUSTERS")
    detected, longest = report_clusters(runs_us)
    base = args.base_tick or detected
    if args.base_tick:
        print(f"  base tick forced to {base:.1f} us")

    ticks = [round(r / base) if r > 0 else -round(-r / base) for r in runs_us]
    ticks = [t for t in ticks if t != 0]

    section("3. FRAME SEGMENTATION")
    gap = args.gap_ticks if args.gap_ticks is not None else auto_gap_ticks(ticks)
    frames = report_frames(ticks, base, gap)

    target = frames[0] if frames else ticks
    section(f"4. ENCODING TESTS  (on frame 1, {len(target)} elements)")
    passed = report_encodings(target)

    final_verdict(passed, longest)

    if args.emit_header:
        section("6. signal.h SNIPPET")
        emit_header(target, base)

    print()


if __name__ == "__main__":
    main()
