"""Back end of FeedbackPlease's "MSN finder" tab.

Reads a CatGT'd + Kilosort4'd run (see msn_data.py), ranks the units by the variance of their spike-time
histogram for one syllable and hands the numbers behind each unit's summary figure to the GUI, which draws them.
Nothing is rendered or saved here.

Protocol (the GUI starts this script and talks to it over stdin/stdout):

  stdin, one command per line, fields separated by TAB:
    LOAD  run_dir  ks_name  good_only(0/1)  max_rate_hz(<=0: no cutoff)  cache_dir
    RANK  label  nbins  min_spikes  n_units(0: all)
    CLASSIFY  bout_gap_s  nonsong_margin_s
    RAWWF  n_spikes   (mean raw waveform of every unit from the .ap.bin; slow, cached)
    FIG   label  unit  pad  want_spec(0/1)
    QUIT

  stdout, binary frames:
    "ARR <name> <dtype> <count>\\n" followed by count items of that dtype (little endian)
    "VAL <name> <value>\\n"          a scalar / short string
    "LOG <text>\\n"                  progress message
    "OK <command>\\n"                the command finished
    "ERR <text>\\n"                  the command failed (the server keeps running)
"""
import os
import sys
from pathlib import Path
import traceback

import numpy as np

# Anything a library prints must not corrupt the frames.
_out = sys.stdout.buffer
sys.stdout = sys.stderr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msn_data import Session, firing_histograms, histogram_variance, rendition_spikes  # noqa: E402

_DTYPES = {'f4': np.float32, 'f8': np.float64, 'i4': np.int32}


def send_line(s):
    _out.write(s.replace('\n', ' ').encode('utf-8', 'replace') + b'\n')


def send_val(name, value):
    send_line(f'VAL {name} {value}')


def send_arr(name, a, dtype='f4'):
    a = np.ascontiguousarray(a, dtype=np.dtype(_DTYPES[dtype]).newbyteorder('<'))
    send_line(f'ARR {name} {dtype} {a.size}')
    _out.write(a.tobytes())


def log(msg):
    send_line(f'LOG {msg}')
    _out.flush()


class State:
    S = None
    units = None
    syl = None          # syllables with a start and an end
    all_spikes = None
    spec_cache = {}     # label -> (freqs, time fraction, dB[freq, time])
    ranked = {}         # label -> {unit id: peak_ms}
    ap = None           # memmap of the run's .ap.bin (samples x 385), None if not found
    st = None           # spike sample index of every spike, and its cluster
    clu = None
    cmap = None         # Kilosort channel -> raw channel
    raw_cache = {}      # unit -> (t_ms, mean waveform in original polarity, half-width ms, sign, n spikes)
    raw_n = 0
    sparse = None       # (sparseness index per unit, labels used), computed on first CLASSIFY


def cmd_load(run_dir, ks_name, good_only, max_rate, cache_dir):
    log(f'Opening {run_dir}')
    kw = {'ks_name': ks_name}
    if cache_dir:
        kw['cache_dir'] = cache_dir
    S = Session(run_dir=run_dir, **kw)
    units = S.units(good_only=bool(int(good_only)), max_rate=float(max_rate) if float(max_rate) > 0 else None)
    log(f'{len(units["cluster_id"])} units ({"good only" if int(good_only) else "all"}); reading syllables '
        f'(the first time per run this scans the NI file and can take a minute)')
    syl = S.syllables()
    ok = np.isfinite(syl['start']) & np.isfinite(syl['end'])
    syl = {k: v[ok] for k, v in syl.items()}
    log(f'{len(syl["label"])} labelled syllables; loading spikes')
    all_spikes = S.spikes_by_unit(units['cluster_id'])
    State.S, State.units, State.syl, State.all_spikes = S, units, syl, all_spikes
    State.raw_cache.clear()
    State.sparse = None
    State.st = np.load(S.ks / 'spike_times.npy').ravel().astype(np.int64)
    State.clu = np.load(S.ks / 'spike_clusters.npy').ravel()
    State.cmap = np.load(S.ks / 'channel_map.npy').ravel()
    ap_path = Path(run_dir) / f'{S.name}_imec0' / f'{S.name}_tcat.imec0.ap.bin'
    State.ap = None
    if ap_path.exists():
        State.ap = np.memmap(ap_path, dtype='int16', mode='r').reshape(-1, 385)
    else:
        log(f'raw data not found ({ap_path}); waveform widths will come from the Kilosort templates')
    State.spec_cache.clear()
    State.ranked.clear()
    labels, counts = np.unique(syl['label'], return_counts=True)
    send_val('name', S.name)
    send_val('duration_s', f'{S.duration:.3f}')
    send_val('n_units', len(units['cluster_id']))
    send_arr('labels', labels, 'i4')
    send_arr('label_counts', counts, 'i4')


def _bouts(gap_s):
    """Song bouts [(start, end)] in NI-clock seconds: the sound pulses (digital line high while the mic is over
    threshold) joined whenever the silence between two of them is at most gap_s."""
    rise, fall = State.S.sound_pulses()
    if len(rise) == 0:
        return np.zeros((0, 2))
    o = np.argsort(rise)
    rise, fall = rise[o], fall[o]
    # a new bout starts where the gap to the running maximum of earlier ends exceeds gap_s
    new = np.r_[True, rise[1:] - np.maximum.accumulate(fall)[:-1] > gap_s]
    first = np.flatnonzero(new)
    return np.column_stack([rise[first], np.maximum.reduceat(fall, first)])


def _complement(epochs, lo, hi):
    """Time in [lo, hi] not inside any of the (sorted, disjoint) epochs."""
    out, t = [], lo
    for a, b in epochs:
        if a > t:
            out.append((t, min(a, hi)))
        t = max(t, b)
    if t < hi:
        out.append((t, hi))
    return np.array([e for e in out if e[1] > e[0]]).reshape(-1, 2)


def _merge(epochs):
    epochs = sorted(map(tuple, epochs))
    out = []
    for a, b in epochs:
        if out and a <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return np.array(out).reshape(-1, 2)


def _mean_inst_rate(spikes, epochs):
    """Time-average over the epochs of the instantaneous rate R(t) = 1 / (enclosing inter-spike interval), as in
    Goldberg & Fee 2010. Before the first and after the last spike R is undefined and counted as 0."""
    total = float(np.sum(epochs[:, 1] - epochs[:, 0])) if len(epochs) else 0.0
    if total <= 0 or len(spikes) < 2:
        return 0.0 if total > 0 else float("nan")
    isi = np.diff(spikes)
    acc = 0.0
    for a, b in epochs:
        lo = max(int(np.searchsorted(spikes, a, "right")) - 1, 0)
        hi = min(int(np.searchsorted(spikes, b, "left")), len(spikes) - 1)  # intervals lo .. hi-1
        if hi <= lo:
            continue
        s, e = spikes[lo:hi], spikes[lo + 1:hi + 1]
        overlap = np.clip(np.minimum(e, b) - np.maximum(s, a), 0, None)
        acc += float(np.sum(overlap / np.maximum(isi[lo:hi], 1e-6)))
    return acc / total


def _count_in(spikes, epochs):
    return int(sum(np.searchsorted(spikes, b, "right") - np.searchsorted(spikes, a, "left") for a, b in epochs))


def _peak_rate(spikes, epochs):
    """99th percentile of R(t) inside the epochs = 1 / (1st percentile of the inter-spike intervals there)."""
    isi = []
    for a, b in epochs:
        s = spikes[(spikes >= a) & (spikes <= b)]
        if len(s) > 1:
            isi.append(np.diff(s))
    isi = np.concatenate(isi) if isi else np.zeros(0)
    return float(1.0 / np.percentile(isi, 1)) if len(isi) >= 2 else float("nan")


def _half_width_ms(t_ms, wf, flip=True):
    """Half-width (ms) of the dominant deflection of a waveform (the larger of its trough and peak; a positive-going
    spike is flipped first): time between the two half-amplitude crossings, linearly interpolated."""
    wf = np.asarray(wf, float)
    t_ms = np.asarray(t_ms, float)
    if flip and abs(wf.max()) > abs(wf.min()):
        wf = -wf
    i = int(np.argmin(wf))
    trough = wf[i]
    if trough >= 0:
        return float("nan")
    half = trough / 2.0
    j = i
    while j > 0 and wf[j] < half:
        j -= 1
    k = i
    while k < len(wf) - 1 and wf[k] < half:
        k += 1
    if wf[j] < half or wf[k] < half:  # ran off the template before crossing
        return float("nan")
    tl = t_ms[j] + (half - wf[j]) * (t_ms[j + 1] - t_ms[j]) / (wf[j + 1] - wf[j])
    tr = t_ms[k - 1] + (half - wf[k - 1]) * (t_ms[k] - t_ms[k - 1]) / (wf[k] - wf[k - 1])
    return float(tr - tl)


_PRE, _POST, _UP = 30, 60, 8  # samples before / after each spike, upsampling factor (30 kHz -> 240 kHz)


def _polarity(unit):
    """+1 if the unit's template on its peak channel is dominated by a trough, -1 if by a peak."""
    tmpl = State.S._unit_template(unit)
    pk = int(np.abs(tmpl).max(0).argmax())
    wf = tmpl[:, pk]
    return (-1 if abs(wf.max()) > abs(wf.min()) else 1), pk


def raw_mean_waveform(unit, n_spikes):
    """Mean raw waveform of a unit on its peak channel: up to n_spikes random spikes, each upsampled 8x and aligned
    on its own dominant deflection at sub-sample precision before averaging, so timing jitter does not widen the mean.
    Returns (t_ms, mean waveform in the original polarity, half-width ms, sign, n spikes) or None."""
    if State.ap is None:
        return None
    from scipy.signal import resample_poly
    S = State.S
    sign, pk = _polarity(unit)
    ch = int(State.cmap[pk])
    sp = State.st[State.clu == unit]
    n_total = State.ap.shape[0]
    sp = sp[(sp > _PRE + 5) & (sp < n_total - _POST - 5)]
    if len(sp) < 5:
        return None
    rng = np.random.default_rng(unit)
    sp = np.sort(rng.choice(sp, size=min(int(n_spikes), len(sp)), replace=False))
    snips = np.stack([State.ap[s - _PRE:s + _POST, ch].astype(np.float64) for s in sp])
    snips -= snips[:, :8].mean(1, keepdims=True)
    up = resample_poly(snips, _UP, 1, axis=1) * sign  # dominant deflection is negative from here on
    c = _PRE * _UP
    lo, hi = c - 6 * _UP, c + 6 * _UP
    idx = lo + up[:, lo:hi].argmin(1)
    a, b = int(0.8e-3 * S.imec_fs * _UP), int(1.6e-3 * S.imec_fs * _UP)
    rows = [up[i, k - a:k + b] for i, k in enumerate(idx) if k - a >= 0 and k + b <= up.shape[1]]
    if not rows:
        return None
    mean = np.mean(rows, axis=0)
    t = (np.arange(len(mean)) - a) / (S.imec_fs * _UP) * 1e3
    return t, mean * sign, _half_width_ms(t, mean, flip=False), sign, len(rows)


def _raw_for(unit, n_spikes):
    if State.raw_n != int(n_spikes):
        State.raw_cache.clear()
        State.raw_n = int(n_spikes)
    if unit not in State.raw_cache:
        State.raw_cache[unit] = raw_mean_waveform(unit, n_spikes)
    return State.raw_cache[unit]


def cmd_rawwf(n_spikes):
    """Raw-data half-width of every unit (slow: reads spikes from the .ap.bin; cached for the session)."""
    n_spikes = int(n_spikes)
    if State.ap is None:
        raise RuntimeError('the run folder has no <run>_imec0/<run>_tcat.imec0.ap.bin to read waveforms from')
    ids = State.units['cluster_id']
    log(f'Measuring mean raw waveforms ({n_spikes} spikes per unit, 8x upsampled, 30 kHz data)')
    width = np.full(len(ids), np.nan)
    pol = np.zeros(len(ids))
    nn = np.zeros(len(ids))
    for i, u in enumerate(ids):
        r = _raw_for(int(u), n_spikes)
        if r is not None:
            width[i], pol[i], nn[i] = r[2], r[3], r[4]
        if (i + 1) % 40 == 0:
            log(f'  raw waveforms: {i + 1} / {len(ids)} units')
    send_arr('raw_units', ids, 'i4')
    send_arr('raw_width_ms', width, 'f8')
    send_arr('raw_polarity', pol, 'f8')
    send_arr('raw_n', nn, 'f8')


_SPARSE_BIN_MS, _SPARSE_MIN_RENDITIONS = 10.0, 30


def sparseness_indices():
    """Entropy-based sparseness index of Goldberg & Fee 2010 (after Lehky et al. 2005), per unit: syllable-aligned
    rate histograms (10 ms bins, mean spikes per rendition) of every syllable label with at least 30 renditions are
    joined and normalized to a probability vector p over all N bins; index = 1 + sum(p log p) / log N. 1 = all
    activity in one bin, 0 = spread evenly. Returns (index per unit, spikes counted per unit, number of syllable labels used)."""
    ids = State.units['cluster_id']
    blocks = []
    nsp = np.zeros(len(ids))
    for lab in np.unique(State.syl['label']):
        sel = State.syl['label'] == lab
        if sel.sum() < _SPARSE_MIN_RENDITIONS:
            continue
        starts, ends = State.syl['start'][sel], State.syl['end'][sel]
        med = float(np.median(ends - starts))
        nb = max(1, int(round(med * 1e3 / _SPARSE_BIN_MS)))
        edges = np.arange(nb + 1) * _SPARSE_BIN_MS
        H = np.zeros((len(ids), nb))
        for i, u in enumerate(ids):
            sp = State.all_spikes.get(int(u))
            if sp is None:
                continue
            rel = rendition_spikes(sp, starts, starts + med, 0.0, 0.0)
            c = np.histogram(np.concatenate(rel) * 1e3, edges)[0]
            H[i] = c / len(starts)
            nsp[i] += c.sum()
        blocks.append(H)
    out = np.full(len(ids), np.nan)
    if not blocks:
        return out, nsp, 0
    H = np.concatenate(blocks, axis=1)
    tot = H.sum(1)
    for i in np.flatnonzero(tot > 0):
        p = H[i] / tot[i]
        nz = p[p > 0]
        out[i] = 1.0 + float(np.sum(nz * np.log(nz))) / np.log(H.shape[1])
    return out, nsp, len(blocks)


def cmd_classify(bout_gap_s, nonsong_margin_s):
    """Per unit: mean rate during song, mean rate far from song, template width and peak rate in song."""
    bout_gap_s, margin = float(bout_gap_s), float(nonsong_margin_s)
    S = State.S
    bouts = _bouts(bout_gap_s)
    song = _merge(bouts)
    near = _merge(np.column_stack([bouts[:, 0] - margin, bouts[:, 1] + margin])) if len(bouts) else np.zeros((0, 2))
    nonsong = _complement(near, 0.0, S.duration)
    song_s, non_s = float(np.sum(song[:, 1] - song[:, 0])), float(np.sum(nonsong[:, 1] - nonsong[:, 0]))
    log(f"{len(bouts)} song bouts ({song_s:.0f} s), {non_s:.0f} s of non-song time (> {margin:g} s from song)")
    ids = State.units["cluster_id"]
    out = {k: np.full(len(ids), np.nan) for k in ("song", "non", "width", "peak", "n_song", "n_non", "pol")}
    for i, u in enumerate(ids):
        sp = State.all_spikes.get(int(u))
        if sp is None:
            continue
        out["song"][i] = _mean_inst_rate(sp, song)
        out["non"][i] = _mean_inst_rate(sp, nonsong)
        out["peak"][i] = _peak_rate(sp, song)
        out["n_song"][i] = _count_in(sp, song)
        out["n_non"][i] = _count_in(sp, nonsong)
        t_wf, wf = S.unit_waveform(int(u))
        out["width"][i] = _half_width_ms(t_wf, wf)
        out["pol"][i] = _polarity(int(u))[0]
    if State.sparse is None:
        State.sparse = sparseness_indices()
        log(f"Sparseness index over {State.sparse[2]} syllable labels (>= {_SPARSE_MIN_RENDITIONS} renditions, {_SPARSE_BIN_MS:g} ms bins)")
    out["sparse"], out["sparse_n"] = State.sparse[0], State.sparse[1]
    send_val("n_bouts", len(bouts))
    send_val("song_s", f"{song_s:.3f}")
    send_val("nonsong_s", f"{non_s:.3f}")
    send_arr("cls_units", ids, "i4")
    for key, name in (("song", "cls_song_rate"), ("non", "cls_nonsong_rate"), ("width", "cls_width_ms"), ("peak", "cls_peak_rate"),
                      ("n_song", "cls_n_song"), ("n_non", "cls_n_nonsong"), ("pol", "cls_polarity"),
                      ("sparse", "cls_sparseness"), ("sparse_n", "cls_sparse_n")):
        send_arr(name, out[key], "f8")


def _renditions(label):
    sel = State.syl['label'] == label
    if not sel.any():
        raise ValueError(f'no renditions with label {label}')
    return State.syl['start'][sel], State.syl['end'][sel]


def cmd_rank(label, nbins, min_spikes, n_units):
    label, nbins, min_spikes, n_units = int(label), int(nbins), int(min_spikes), int(n_units)
    starts, ends = _renditions(label)
    centers, H, n, ids = firing_histograms(State.all_spikes, starts, ends, nbins)
    _, var = histogram_variance(centers, H)
    keep = n >= min_spikes
    order = np.argsort(var[keep])
    if n_units > 0:
        order = order[:n_units]
    top = ids[keep][order]
    peak = centers[H[keep][order].argmax(1)] if len(order) else np.zeros(0)
    State.ranked[label] = dict(zip(top.tolist(), peak.tolist()))
    send_val('n_renditions', len(starts))
    send_val('median_ms', f'{np.median(ends - starts) * 1e3:.3f}')
    send_arr('rank_units', top, 'i4')
    send_arr('rank_var', var[keep][order], 'f8')
    send_arr('rank_nspikes', n[keep][order], 'f8')
    send_arr('rank_peak_ms', peak, 'f8')


def cmd_fig(label, unit, pad, want_spec):
    label, unit, pad = int(label), int(unit), float(pad)
    S = State.S
    starts, ends = _renditions(label)
    dur = ends - starts
    if label not in State.spec_cache:
        log(f'Averaging spectrograms of {len(starts)} renditions of syllable {label}')
        State.spec_cache[label] = S.mean_spectrogram(starts, ends)
    f, t, spec = State.spec_cache[label]
    spikes = State.all_spikes.get(unit)
    if spikes is None:
        spikes = S.spike_times(unit)
    pad_s = pad * np.median(dur)
    rel = [x * 1e3 for x in rendition_spikes(spikes, starts, ends, pad_s, pad_s)]  # ms after onset, unwarped
    t_wf, wf = S.unit_waveform(unit)
    xy, shank, amp, peak = S.unit_footprint(unit)

    send_val('n_renditions', len(starts))
    send_val('median_ms', f'{np.median(dur) * 1e3:.3f}')
    peak_ms = State.ranked.get(label, {}).get(unit)
    if peak_ms is None:  # not from a variance ranking: the unit's own histogram peak
        c, H, n, _ = firing_histograms({unit: spikes}, starts, ends, 20)
        peak_ms = float(c[H[0].argmax()]) if n[0] > 0 else float('nan')
    send_val('peak_ms', peak_ms)
    send_val('peak_channel', peak)
    send_val('peak_shank', int(shank[peak]))
    send_val('peak_y_um', f'{xy[peak, 1]:.0f}')
    if int(want_spec):
        send_arr('spec_freq_hz', f, 'f4')
        send_arr('spec_time_frac', t, 'f4')
        send_arr('spec_db', spec, 'f4')  # [freq, time], row-major
    send_arr('end_ms', dur * 1e3, 'f4')
    send_arr('spike_ms', np.concatenate(rel) if rel else np.zeros(0), 'f4')
    send_arr('spike_rendition', np.concatenate([np.full(len(x), i) for i, x in enumerate(rel)]) if rel else np.zeros(0), 'i4')
    send_arr('wf_t_ms', t_wf, 'f4')
    raw = _raw_for(unit, State.raw_n or 150) if State.ap is not None else None
    if raw is not None:
        send_arr('raw_t_ms', raw[0][::4], 'f4')
        send_arr('raw_wf', raw[1][::4], 'f4')
        send_val('raw_width_ms', f'{raw[2]:.4f}')
        send_val('raw_n', raw[4])
    send_arr('wf', wf, 'f4')
    send_arr('fp_xy', xy, 'f4')
    send_arr('fp_shank', shank, 'i4')
    send_arr('fp_amp', amp, 'f4')


COMMANDS = {'LOAD': (cmd_load, 5), 'RANK': (cmd_rank, 4), 'CLASSIFY': (cmd_classify, 2), 'RAWWF': (cmd_rawwf, 1), 'FIG': (cmd_fig, 4)}


def main():
    send_line('LOG msn_server ready')
    _out.flush()
    for line in sys.stdin:
        parts = line.rstrip('\r\n').split('\t')
        name = parts[0].upper()
        if name == 'QUIT':
            break
        try:
            fn, nargs = COMMANDS[name]
            if len(parts) - 1 != nargs:
                raise ValueError(f'{name} takes {nargs} arguments, got {len(parts) - 1}')
            fn(*parts[1:])
            send_line(f'OK {name}')
        except Exception as e:  # keep serving; the GUI shows the message
            traceback.print_exc(file=sys.stderr)
            send_line(f'ERR {name}: {type(e).__name__}: {e}')
        _out.flush()


if __name__ == '__main__':
    main()
