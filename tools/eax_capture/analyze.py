#!/usr/bin/env python3
"""Compares EAX recordings from real hardware (eaxcap.exe) with x86recomp's
renderings of the same cases (eax_render).

    python3 -I analyze.py --hw capture/ --sim render/ [--provider eax2]
                          [--cases LIST] [--tsv report.tsv] [--plot plots/]

Either set may be given alone. Each recording is aligned on its sync click
(the first sound after the silent pre-roll; the stimulus starts lead_s
after it); sweeps are deconvolved into impulse responses. Per case:
reverb decay (RT60 from the energy decay curve, T20, per octave band;
EDT), reverb onset after the direct sound, early/late reverb and direct
levels, direct-path band levels (obstruction / occlusion shelves, measured
dry), and per-channel levels for noise and tone stimuli. Relative columns
compare a case with its group's reference case of the same set. Needs
numpy; writes SVG plots itself.
"""

import argparse
import fnmatch
import json
import math
import os
import struct
import sys

import numpy as np

BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]
PRE_S = 0.005        # IR samples kept before the stimulus start
DIRECT_S = (0.0005, 0.0015)  # direct sound window around its peak
EARLY_S = 0.05       # early reverb: this long from the reverb onset

# Reference case of each case family (same provider and set).
REFS = [
    ("obst_", "src_dry_ref"), ("occl_dry_", "src_dry_ref"), ("src_dry_", "src_dry_ref"),
    ("occl_wet_", "src_wet_ref"), ("excl_", "src_wet_ref"), ("src_wet_", "src_wet_ref"),
    ("dist_air100_", "dist_air100_1"), ("dist_rolloff1_", "dist_rolloff1_1"),
    ("dist_dry_", "dist_dry_1"), ("dist_", "dist_1"), ("pan_", "pan_az0"),
    ("eax1_", "env_00_generic"),
]
LISTENER_REF = "listener_base"


# ---- input ------------------------------------------------------------------------

def read_wav(path):
    """(rate, float64 frames x channels) of a PCM 16/24/32-bit or float WAV."""
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError("%s: not a WAV file" % path)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
            if fmt[0] == 0xFFFE and len(body) >= 26:
                fmt = (struct.unpack_from("<H", body, 24)[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if not fmt or data is None:
        raise ValueError("%s: no fmt or data chunk" % path)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], "<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data[:len(data) // 2 * 2], "<i2") / 32768.0
    elif tag == 1 and bits == 24:
        a = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data[:len(data) // 4 * 4], "<i4") / 2147483648.0
    else:
        raise ValueError("%s: unsupported WAV format %d/%d-bit" % (path, tag, bits))
    return rate, x[:len(x) // ch * ch].reshape(-1, ch)


class Rec:
    def __init__(self, root, tag, base):
        self.dir = os.path.join(root, tag)
        with open(os.path.join(self.dir, base + ".json"), encoding="utf-8") as f:
            self.meta = json.load(f)
        self.rate, x = read_wav(os.path.join(self.dir, base + ".wav"))
        self.x = x if x.shape[1] == 2 else np.repeat(x[:, :1], 2, axis=1)
        self.case = self.meta["case"]
        self.stim = self.meta["stimulus"]


_stim_cache = {}


def stimulus(rec):
    path = os.path.join(rec.dir, "stim_%s.wav" % rec.stim)
    if path not in _stim_cache:
        _stim_cache[path] = read_wav(path)[1][:, 0]
    return _stim_cache[path]


def discover(root):
    """{(tag, case, stim): base name} of the recordings under root."""
    found = {}
    if not root:
        return found
    for tag in sorted(os.listdir(root)):
        d = os.path.join(root, tag)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if name.endswith(".json") and os.path.exists(os.path.join(d, name[:-5] + ".wav")):
                base = name[:-5]
                case, _, stim = base.rpartition(".")
                found[(tag, case, stim)] = base
    return found


# ---- signal helpers --------------------------------------------------------------------

def find_sync(x, rate):
    """Index of the sync click: the first sample above the pre-roll's noise
    (as eaxcap.c finds it), refined to the peak within 2 ms."""
    a = np.abs(x).max(axis=1)
    n0 = rate // 5
    thr = max(a[:n0].max() * 8, 10 ** (-60 / 20))
    hit = np.nonzero(a[n0:] > thr)[0]
    if not len(hit):
        return None
    i = n0 + int(hit[0])
    return i + int(np.argmax(a[i:i + int(0.002 * rate) + 1]))


def band_mask(n, rate, lo, hi):
    """rfft-bin mask passing lo..hi Hz, with raised-cosine edges a third of
    an octave wide (adjacent bands sharing an edge sum to 1)."""
    lf = np.log2(np.maximum(np.fft.rfftfreq(n, 1.0 / rate), 1e-9))

    def step(edge):  # 0 below edge, 1 above
        t = np.clip((lf - math.log2(edge)) * 3 + 0.5, 0, 1)
        return 0.5 - 0.5 * np.cos(math.pi * t)

    return step(lo) * (1 - step(hi))


def band_masks(n, rate):
    return {fc: band_mask(n, rate, fc / math.sqrt(2), fc * math.sqrt(2)) for fc in BANDS}


def moving_average(x, win):
    """Centred moving average of a 1-D array (same length)."""
    if win <= 1:
        return x
    c = np.concatenate([[0.0], np.cumsum(x)])
    lo = np.clip(np.arange(len(x)) - win // 2, 0, len(x))
    hi = np.clip(lo + win, 0, len(x))
    return (c[hi] - c[lo]) / win


def fft_size(n):
    return 1 << int(math.ceil(math.log2(max(n, 2))))


def band_filter(x, rate):
    """{band: x filtered to that octave} (x: samples x channels)."""
    n = fft_size(len(x) + rate // 10)  # room for the filters' ringing
    spec = np.fft.rfft(x, n, axis=0)
    out = {}
    for fc, m in band_masks(n, rate).items():
        out[fc] = np.fft.irfft(spec * m[:, None], n, axis=0)[:len(x)]
    return out


def db(e, ref=1.0):
    return 10 * math.log10(e / ref) if e > 0 and ref > 0 else float("nan")


def impulse_response(rec, t0):
    """IR (samples x 2) starting PRE_S before the stimulus start, scaled so
    a unity path is 1."""
    pre = int(PRE_S * rec.rate)
    seg = rec.x[max(t0 - pre, 0):]
    s = stimulus(rec)
    if rec.stim == "impulse":
        return seg[:len(seg)] / np.abs(s).max(), pre
    n = fft_size(len(seg) + len(s))
    S = np.fft.rfft(s, n)
    p = np.abs(S) ** 2
    inv = np.conj(S) / (p + 1e-4 * p.max())
    ir = np.fft.irfft(np.fft.rfft(seg, n, axis=0) * inv[:, None], n, axis=0)
    return ir[:max(len(seg) - len(s), pre + 1)], pre


def noise_floor(e):
    """Mean energy of the last tenth of a response (by then only noise is
    left: recordings run 1.5 decay times past the stimulus)."""
    return float(e[-max(len(e) // 10, 1):].mean())


def energy(e, noise):
    """Energy of e above the noise floor."""
    return float(e.sum()) - noise * len(e)


def decay_times(e, rate):
    """(RT60, EDT, Schroeder curve in dB) of energy e. The noise floor is
    subtracted, the integration stops where the decay (smoothed over 30 ms)
    reaches 5 dB above the floor, and the energy past that point is added
    back from the decay's own slope (Lundeby). RT60 is T20 (-5..-25 dB)
    when the decay stands 35 dB clear of the noise, T10 (-5..-15) at 25 dB,
    else NaN; EDT is 0..-10 dB."""
    nan = float("nan")
    if len(e) < rate // 20 or not e.any():
        return nan, nan, None
    noise = noise_floor(e)
    smooth = moving_average(e, max(int(0.03 * rate), 1))
    peak = int(np.argmax(smooth))
    rng = 10 * math.log10(smooth[peak] / noise) if noise > 0 else 200.0
    low = np.nonzero(smooth[peak:] < noise * 10 ** 0.5)[0]
    end = peak + int(low[0]) if len(low) else len(e)
    if end - peak < rate // 100:
        return nan, nan, None
    edc = np.cumsum((e[:end] - noise)[::-1])[::-1]
    if end < len(e):
        # Energy beyond `end`, from the envelope's slope between 5 dB under
        # its peak and 10 dB over the noise.
        lvl = 10 * np.log10(np.maximum(smooth[peak:end], 1e-30) / smooth[peak])
        sel = np.nonzero((lvl <= -5) & (lvl >= -(rng - 10)))[0]
        if len(sel) > 10:
            k = np.polyfit(sel / rate, lvl[sel], 1)[0]  # dB per second
            if k < 0:
                edc += max(smooth[end] - noise, 0.0) * rate * 10 / (-k * math.log(10))
    if edc[0] <= 0:
        return nan, nan, None
    curve = 10 * np.log10(np.maximum(edc / edc[0], 1e-30))
    t = np.arange(len(curve)) / rate

    def fit(hi, lo):
        # The first stretch from hi down to lo.
        below = np.nonzero(curve < lo)[0]
        if not len(below):
            return nan
        sel = np.nonzero(curve[:below[0]] <= hi)[0]
        if len(sel) < 10:
            return nan
        slope = np.polyfit(t[sel], curve[sel], 1)[0]
        return -60.0 / slope if slope < 0 else nan

    rt = fit(-5, -25) if rng >= 35 else fit(-5, -15) if rng >= 25 else nan
    return rt, fit(0, -10) if rng >= 15 else nan, curve


# ---- per-recording metrics -------------------------------------------------------------

def analyze(rec):
    m = {}
    rate = rec.rate
    sync = find_sync(rec.x, rate)
    if sync is None:
        m["error"] = "no sync click"
        return m, None
    started = rec.meta.get("started_at_frame")
    if started is not None:
        m["latency_ms"] = 1000.0 * (sync - started) / rate
    t0 = sync + int(round(rec.meta["lead_s"] * rate))
    s = stimulus(rec)

    if rec.stim in ("noise", "tone"):
        a, b = t0 + int(0.1 * rate), t0 + len(s) - int(0.05 * rate)
        seg, ref = rec.x[a:b], s[a - t0:b - t0]
        er = float(np.mean(ref ** 2))
        m["level_L"] = db(float(np.mean(seg[:, 0] ** 2)), er)
        m["level_R"] = db(float(np.mean(seg[:, 1] ** 2)), er)
        m["pan_db"] = m["level_R"] - m["level_L"]
        m["level"] = db(float(np.mean(seg ** 2)), er)
        if rec.stim == "noise":
            bs, br = band_filter(seg, rate), band_filter(ref[:, None], rate)
            for fc in BANDS:
                m["band_%d" % fc] = db(float(np.mean(bs[fc] ** 2)), float(np.mean(br[fc] ** 2)))
        return m, None

    ir, pre = impulse_response(rec, t0)
    e = (ir ** 2).sum(axis=1)
    w = int(0.002 * rate)
    d = pre - w + int(np.argmax(e[pre - w:pre + w + 1]))
    m["direct_dt_ms"] = 1000.0 * (d - pre) / rate
    d0, d1 = d - int(DIRECT_S[0] * rate), d + int(DIRECT_S[1] * rate) + 1
    ed = ir[d0:d1]
    m["direct_db"] = db(float((ed ** 2).sum()) / 2)
    m["direct_pan_db"] = db(float((ed[:, 1] ** 2).sum()), float((ed[:, 0] ** 2).sum()))
    # The whole response per band (for dry cases: the direct path's filter).
    bands = band_filter(ir[max(d - pre, 0):], rate)
    for fc in BANDS:
        eb = (bands[fc] ** 2).sum(axis=1)
        m["band_%d" % fc] = db(energy(eb, noise_floor(eb)) / 2)

    wet = e.copy()
    wet[:d1] = 0
    noise = noise_floor(wet)
    m["noise_db"] = db(noise / 2)
    ew = energy(wet[d1:], noise)
    m["wet_db"] = db(ew / 2)
    m["wetdry_db"] = m["wet_db"] - m["direct_db"]
    win = max(int(0.0005 * rate), 1)
    env = moving_average(wet, win)
    peak = int(np.argmax(env))
    curves = {}
    # Analyze the reverb only when it stands clear of the noise.
    if ew > 0 and env[peak] > 30 * noise and m["wetdry_db"] > -80:
        # Onset: the first rise to -20 dB of the reverb's envelope peak
        # (and well above the noise) once the direct sound's own tail and
        # the deconvolution's ringing have fallen below that level.
        thr = max(env[peak] * 0.01, noise * 3)
        below = np.nonzero(env[d1:peak + 1] < thr)[0]
        start = d1 + int(below[0]) if len(below) else d1
        on = np.nonzero(env[start:] > thr)[0]
        onset = start + int(on[0]) if len(on) else d1
        m["onset_ms"] = 1000.0 * (onset - d) / rate
        early_end = onset + int(EARLY_S * rate)
        m["early_db"] = db(energy(wet[onset:early_end], noise) / 2)
        m["late_db"] = db(energy(wet[early_end:], noise) / 2)
        rt, edt, curve = decay_times(wet[onset:], rate)
        m["rt"], m["edt"] = rt, edt
        curves["all"] = curve
        wb = band_filter(ir[d1:], rate)
        for fc in BANDS:
            eb = (wb[fc] ** 2).sum(axis=1)
            rt, _, curve = decay_times(eb[max(onset - d1, 0):], rate)
            m["rt_%d" % fc] = rt
            curves[fc] = curve
        mid = [v for v in (m["rt_500"], m["rt_1000"]) if not math.isnan(v)]
        m["rt_mid"] = float(np.mean(mid)) if mid else float("nan")
    return m, {"ir": ir, "pre": pre, "rate": rate, "curves": curves}


def add_relative(results):
    """rel_* = metric minus the family reference's, within one set."""
    for (tag, case, stim), m in results.items():
        if "error" in m:
            continue
        ref = None
        for prefix, r in REFS:
            if case.startswith(prefix) and case != r:
                ref = r
                break
        if ref is None and m.get("_group") == "listener" and case != LISTENER_REF:
            ref = LISTENER_REF
        rm = results.get((tag, ref, stim)) if ref else None
        if not rm or "error" in rm:
            continue
        m["_ref"] = ref
        for k in list(m):
            if k.startswith(("direct_db", "wet_db", "band_", "level", "rt")) and k in rm:
                if isinstance(m[k], float) and isinstance(rm[k], float):
                    m["rel_" + k] = m[k] - rm[k]


# ---- report -------------------------------------------------------------------------

COLUMNS = {
    # group: [(header, metric, format)]
    "ir": [("RT mid", "rt_mid", "%.2f"), ("RT 4k", "rt_4000", "%.2f"),
           ("RT 125", "rt_125", "%.2f"), ("EDT", "edt", "%.2f"),
           ("onset ms", "onset_ms", "%.1f"), ("early dB", "early_db", "%.1f"),
           ("late dB", "late_db", "%.1f"), ("wet-dir", "wetdry_db", "%.1f")],
    "dry": [("dir dB", "direct_db", "%.1f"), ("d250", "rel_band_250", "%.1f"),
            ("d1k", "rel_band_1000", "%.1f"), ("d4k", "rel_band_4000", "%.1f"),
            ("d8k", "rel_band_8000", "%.1f"), ("wet dB", "wet_db", "%.1f")],
    "src": [("dir dB", "direct_db", "%.1f"), ("rel dir", "rel_direct_db", "%.1f"),
            ("wet dB", "wet_db", "%.1f"), ("rel wet", "rel_wet_db", "%.1f"),
            ("RT mid", "rt_mid", "%.2f"), ("onset ms", "onset_ms", "%.1f")],
    "noise": [("level", "level", "%.1f"), ("L", "level_L", "%.1f"), ("R", "level_R", "%.1f"),
              ("R-L", "pan_db", "%.1f"), ("rel", "rel_level", "%.1f"),
              ("125", "band_125", "%.1f"), ("8k", "band_8000", "%.1f")],
}


def column_set(case, stim, meta):
    if stim in ("noise", "tone"):
        return "noise"
    if case.startswith(("obst_", "occl_dry_", "src_dry_")):
        return "dry"
    if meta.get("group") in ("source", "distance"):
        return "src"
    return "ir"


def fmt(v, f):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    return f % v


def report(hw, sim, keys, metas, out):
    sets = [n for n, r in (("hw", hw), ("sim", sim)) if r]
    both = len(sets) == 2
    out.write("Columns: %s (latency is the sync click's, from sample start).\n" % (
        "hardware/render; d* and rel are relative to the family's reference case"
        if both else sets[0]))
    last = None
    for key in keys:
        tag, case, stim = key
        cols = COLUMNS[column_set(case, stim, metas[key])]
        head = (tag, cols)
        if head != last:
            out.write("\n[%s]\n%-34s" % (tag, "case.stimulus"))
            for h, _, _ in cols:
                out.write(" %15s" % h if both else " %9s" % h)
            out.write("\n")
            last = head
        out.write("%-34s" % ("%s.%s" % (case, stim)))
        for _, k, f in cols:
            if both:
                a, b = hw.get(key, {}), sim.get(key, {})
                cell = "%s/%s" % (fmt(a.get(k), f), fmt(b.get(k), f))
                out.write(" %15s" % cell)
            else:
                r = hw or sim
                out.write(" %9s" % fmt(r.get(key, {}).get(k), f))
        notes = []
        for name, r in (("hw", hw), ("sim", sim)):
            if r and "error" in r.get(key, {"error": "missing"}):
                notes.append("%s: %s" % (name, r.get(key, {}).get("error", "missing")))
        if notes:
            out.write("  (" + "; ".join(notes) + ")")
        out.write("\n")

    if both:
        out.write("\nSummary over cases present in both sets (hardware minus render):\n")
        for k, label in (("rt_mid", "RT mid (s)"), ("rt_4000", "RT 4k (s)"),
                         ("onset_ms", "reverb onset (ms)"), ("early_db", "early dB"),
                         ("late_db", "late dB"), ("direct_db", "direct dB"),
                         ("rel_band_4000", "dry 4k shelf vs ref (dB)")):
            d = [hw[x][k] - sim[x][k] for x in keys
                 if x in hw and x in sim and isinstance(hw[x].get(k), float)
                 and isinstance(sim[x].get(k), float)
                 and not math.isnan(hw[x][k]) and not math.isnan(sim[x][k])]
            if d:
                out.write("  %-26s n=%3d  median %+7.2f  mean |d| %6.2f  max |d| %6.2f\n" % (
                    label, len(d), float(np.median(d)), float(np.mean(np.abs(d))),
                    float(np.max(np.abs(d)))))


def write_tsv(path, sets):
    with open(path, "w", encoding="utf-8") as f:
        f.write("set\tprovider\tcase\tstimulus\tmetric\tvalue\n")
        for name, results in sets:
            for (tag, case, stim), m in sorted(results.items()):
                for k, v in sorted(m.items()):
                    if k.startswith("_"):
                        continue
                    f.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (name, tag, case, stim, k, v))


# ---- SVG plots ----------------------------------------------------------------------

def nice_ticks(lo, hi):
    """Ticks at 1, 2 or 5 times a power of ten, about five of them."""
    raw = (hi - lo) / 5
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 5, 10) if m * mag >= raw)
    first = math.ceil(lo / step) * step
    return [round(first + i * step, 10) for i in range(int((hi - first) / step + 1e-9) + 1)]


def svg_plot(path, title, series, xlabel, ylabel, xlim, ylim, logx=False):
    """series: [(label, colour, dash, xs, ys)]."""
    W, H, L, R, T, B = 640, 400, 60, 140, 30, 40
    x0, x1 = xlim
    y0, y1 = ylim

    def X(v):
        if logx:
            v, a, b = math.log10(max(v, 1e-9)), math.log10(x0), math.log10(x1)
        else:
            a, b = x0, x1
        return L + (v - a) / (b - a) * (W - L - R)

    def Y(v):
        return T + (y1 - min(max(v, y0), y1)) / (y1 - y0) * (H - T - B)

    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'font-family="sans-serif" font-size="11">' % (W, H),
         '<rect width="100%" height="100%" fill="white"/>',
         '<text x="%d" y="18" font-size="13">%s</text>' % (L, title)]
    ticks = [x0 * 10 ** i for i in range(6) if x0 * 10 ** i <= x1] if logx else \
        nice_ticks(x0, x1)
    for t in ticks:
        o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#ddd"/>' % (X(t), T, X(t), H - B))
        o.append('<text x="%.1f" y="%d" text-anchor="middle">%g</text>' % (X(t), H - B + 14, t))
    for t in nice_ticks(y0, y1):
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#ddd"/>' % (L, Y(t), W - R, Y(t)))
        o.append('<text x="%d" y="%.1f" text-anchor="end">%g</text>' % (L - 4, Y(t) + 4, t))
    o.append('<text x="%d" y="%d" text-anchor="middle">%s</text>' % ((L + W - R) // 2, H - 6, xlabel))
    o.append('<text x="12" y="%d" transform="rotate(-90 12 %d)" text-anchor="middle">%s</text>'
             % ((T + H - B) // 2, (T + H - B) // 2, ylabel))
    for i, (label, colour, dash, xs, ys) in enumerate(series):
        step = max(len(xs) // 800, 1)
        pts = " ".join("%.1f,%.1f" % (X(a), Y(b)) for a, b in zip(xs[::step], ys[::step])
                       if x0 <= a <= x1 and not math.isnan(b))
        o.append('<polyline fill="none" stroke="%s" stroke-width="1.3" %s points="%s"/>' % (
            colour, 'stroke-dasharray="5,3"' if dash else "", pts))
        o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" %s/>' % (
            W - R + 8, T + 14 * i + 6, W - R + 28, T + 14 * i + 6, colour,
            'stroke-dasharray="5,3"' if dash else ""))
        o.append('<text x="%d" y="%d">%s</text>' % (W - R + 32, T + 14 * i + 10, label))
    o.append("</svg>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(o))


COLOURS = {"all": "#000000", 125: "#7b3294", 500: "#2166ac", 1000: "#1b9e77",
           4000: "#d95f02", 8000: "#e7298a"}


def plot_case(dirname, key, data):
    """data: {set name: analysis extras}."""
    tag, case, stim = key
    os.makedirs(os.path.join(dirname, tag), exist_ok=True)
    base = os.path.join(dirname, tag, "%s.%s" % (case, stim))
    series, tmax = [], 0.5
    for name, x in data.items():
        for band, curve in x["curves"].items():
            if curve is None or band not in COLOURS:
                continue
            t = np.arange(len(curve)) / x["rate"]
            tmax = max(tmax, float(t[-1]))
            series.append(("%s %s" % (name, band), COLOURS[band], name == "sim", t, curve))
    if series:
        svg_plot(base + ".edc.svg", "%s %s.%s: energy decay (reverb only)" % (tag, case, stim),
                 series, "seconds", "dB", (0, tmax), (-70, 0))
    series = []
    for name, x in data.items():
        ir = x["ir"][x["pre"] - int(0.001 * x["rate"]):]
        n = 1 << int(math.ceil(math.log2(max(len(ir), 2))))
        spec = np.abs(np.fft.rfft(ir.mean(axis=1), n))
        f = np.fft.rfftfreq(n, 1.0 / x["rate"])
        # 1/6-octave smoothing on a log grid.
        grid = np.geomspace(20, min(20000, x["rate"] / 2 * 0.95), 300)
        lvl = []
        for g in grid:
            sel = (f >= g / 2 ** (1 / 12)) & (f <= g * 2 ** (1 / 12))
            lvl.append(10 * math.log10(max(float(np.mean(spec[sel] ** 2)), 1e-30))
                       if sel.any() else float("nan"))
        series.append((name, "#000000" if name == "hw" else "#d95f02", name == "sim",
                       list(grid), lvl))
    svg_plot(base + ".freq.svg", "%s %s.%s: magnitude (whole response)" % (tag, case, stim),
             series, "Hz", "dB", (20, 20000), (-60, 10), logx=True)


# ---- main ---------------------------------------------------------------------------

def run_set(root, keys, plots):
    results = {}
    for key in keys:
        tag, case, stim = key
        try:
            rec = Rec(root, tag, "%s.%s" % (case, stim))
            m, extra = analyze(rec)
            m["_group"] = rec.meta.get("group")
        except (OSError, ValueError, KeyError) as e:
            m, extra = {"error": str(e)}, None
        results[key] = m
        if extra is not None:
            plots.setdefault(key, {})[os.path.basename(os.path.normpath(root))] = extra
    add_relative(results)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--hw", help="eaxcap.exe output directory (hardware)")
    ap.add_argument("--sim", help="eax_render output directory (x86recomp)")
    ap.add_argument("--provider", help="provider tags, comma-separated (default: all)")
    ap.add_argument("--cases", help="case patterns, comma-separated, * wildcards")
    ap.add_argument("--tsv", help="write every metric to this TSV file")
    ap.add_argument("--plot", help="write SVG plots (EDC, magnitude) into this directory")
    a = ap.parse_args()
    if not a.hw and not a.sim:
        ap.error("give --hw and/or --sim")

    hw_files, sim_files = discover(a.hw), discover(a.sim)
    keys = set(hw_files) | set(sim_files)
    if a.hw and a.sim:
        keys = set(hw_files) & set(sim_files)
        only = (set(hw_files) ^ set(sim_files))
        if only:
            print("note: %d recordings exist in one set only and are skipped" % len(only))
    if a.provider:
        keys = {k for k in keys if k[0] in a.provider.split(",")}
    if a.cases:
        pats = a.cases.split(",")
        keys = {k for k in keys if any(fnmatch.fnmatchcase(k[1], p) for p in pats)}
    if not keys:
        print("no recordings to analyze", file=sys.stderr)
        return 1

    # Case order as in the case list: by the manifests' file order is not
    # stable, so sort by (provider, group order, name).
    group_order = ["level", "env", "eax1", "listener", "source", "distance", "position"]
    metas = {}
    for k in keys:
        root = a.hw if k in hw_files else a.sim
        with open(os.path.join(root, k[0], "%s.%s.json" % (k[1], k[2])), encoding="utf-8") as f:
            metas[k] = json.load(f)
    keys = sorted(keys, key=lambda k: (k[0], group_order.index(metas[k].get("group"))
                                      if metas[k].get("group") in group_order else 99, k[1], k[2]))

    plots = {}
    hw = run_set(a.hw, keys, plots) if a.hw else {}
    sim = run_set(a.sim, keys, plots) if a.sim else {}
    if a.hw and a.sim:
        # Name the plot series by role, not directory.
        hwn, simn = (os.path.basename(os.path.normpath(p)) for p in (a.hw, a.sim))
        plots = {k: {("hw" if n == hwn else "sim"): v for n, v in d.items()} for k, d in plots.items()}
    report(hw, sim, keys, metas, sys.stdout)
    if a.tsv:
        write_tsv(a.tsv, [(n, r) for n, r in (("hw", hw), ("sim", sim)) if r])
        print("\nwrote %s" % a.tsv)
    if a.plot:
        for key, data in plots.items():
            plot_case(a.plot, key, data)
        print("wrote plots to %s" % a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
