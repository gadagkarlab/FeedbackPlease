"""Back end of FeedbackPlease's "MSN finder" tab.

Reads a CatGT'd + Kilosort4'd run (see msn_data.py), ranks the units by the variance of their spike-time
histogram for one syllable and hands the numbers behind each unit's summary figure to the GUI, which draws them.
Nothing is rendered or saved here.

Protocol (the GUI starts this script and talks to it over stdin/stdout):

  stdin, one command per line, fields separated by TAB:
    LOAD  run_dir  ks_name  good_only(0/1)  max_rate_hz(<=0: no cutoff)  cache_dir
    RANK  label  nbins  min_spikes  n_units(0: all)
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
    State.spec_cache.clear()
    State.ranked.clear()
    labels, counts = np.unique(syl['label'], return_counts=True)
    send_val('name', S.name)
    send_val('duration_s', f'{S.duration:.3f}')
    send_val('n_units', len(units['cluster_id']))
    send_arr('labels', labels, 'i4')
    send_arr('label_counts', counts, 'i4')


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
    send_val('peak_ms', State.ranked.get(label, {}).get(unit, float('nan')))
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
    send_arr('wf', wf, 'f4')
    send_arr('fp_xy', xy, 'f4')
    send_arr('fp_shank', shank, 'i4')
    send_arr('fp_amp', amp, 'f4')


COMMANDS = {'LOAD': (cmd_load, 5), 'RANK': (cmd_rank, 4), 'FIG': (cmd_fig, 4)}


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
