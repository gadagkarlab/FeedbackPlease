"""Loaders for one CatGT'd + Kilosort4'd run: unit spikes, syllable times/labels, audio.

Everything is returned in seconds on the NI (mic) clock of the concatenated tcat file.
"""
from pathlib import Path

import csv

import numpy as np
from scipy.interpolate import interp1d
from scipy.signal import spectrogram

DEFAULT_RUN = Path.home()
LABEL_LINES = (5, 6, 7)  # digital lines holding the syllable index, line 5 = LSB
MIC_CHAN = 0
SEG_LINE = 4  # digital line high while the audio is over threshold: rise = syllable onset, fall = offset


def read_meta(path):
    meta = {}
    for line in Path(path).read_text().splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            meta[k.lstrip('~')] = v
    return meta


def rendition_spikes(spike_times, starts, ends, pre=0.0, post=0.0):
    """Spike times per rendition: a list of R arrays, the rth holding the spikes (s) within
    [starts[r] - pre, ends[r] + post], relative to starts[r]. `spike_times` must be sorted;
    `pre`/`post` can be scalars or one value per rendition.
    """
    lo = np.searchsorted(spike_times, np.asarray(starts) - pre)
    hi = np.searchsorted(spike_times, np.asarray(ends) + post)
    return [spike_times[a:b] - s for a, b, s in zip(lo, hi, starts)]


def firing_histograms(spikes_by_unit, starts, ends, nbins=20, pad=0.0):
    """Relative-frequency histogram of each unit's spike times across renditions, unwarped: ms after each
    rendition's onset, over [-pad * med, (1 + pad) * med] where med is the median syllable duration. Every
    rendition contributes the same fixed window, so longer renditions are cut at med (+ pad) and shorter ones
    include some post-offset time.

    Returns (bin centers in ms, H[unit, bin] with rows summing to 1, spike counts per unit, unit ids). Units with
    no spikes in the window get an all-zero row.
    """
    starts, ends = np.asarray(starts), np.asarray(ends)
    med = np.median(ends - starts)
    edges = np.linspace(-pad * med, (1 + pad) * med, nbins + 1) * 1e3
    ids = list(spikes_by_unit)
    counts = np.zeros((len(ids), nbins))
    for i, u in enumerate(ids):
        rel = rendition_spikes(spikes_by_unit[u], starts, starts + med, pad * med, pad * med)
        counts[i] = np.histogram(np.concatenate(rel) * 1e3, edges)[0]
    n = counts.sum(1)
    return (edges[:-1] + edges[1:]) / 2, counts / np.maximum(n, 1)[:, None], n, np.array(ids)


def histogram_variance(centers, H):
    """mean and variance of the spike-time distribution described by each row of H (same units as `centers`)."""
    mean = H @ centers
    return mean, H @ centers ** 2 - mean ** 2


class Session:
    def __init__(self, run_dir=DEFAULT_RUN, ks_name='imec0_ks4', cache_dir=Path(__file__).parent / 'cache'):
        run = Path(run_dir)
        name = run.name.removeprefix('catgt_')  # e.g. Choc59_d1.1_g0
        imec = run / f'{name}_imec0'
        self.name, self.ks = name, imec / ks_name
        self.cache = Path(cache_dir)
        self.cache.mkdir(parents=True, exist_ok=True)

        ni_meta = read_meta(run / f'{name}_tcat.nidq.meta')
        self.imec_fs = float(read_meta(imec / f'{name}_tcat.imec0.ap.meta')['imSampRate'])
        self.ni_fs = float(ni_meta['niSampRate'])
        self.volts_per_bit = float(ni_meta['niAiRangeMax']) / float(ni_meta['niMaxInt'])
        self.ni = np.memmap(run / f'{name}_tcat.nidq.bin', dtype='int16', mode='r').reshape(-1, int(ni_meta['nSavedChans']))
        self.duration = len(self.ni) / self.ni_fs
        st = (run / f'{name}_tcat.nidq.bin').stat()
        self._tag = f'{name}_{st.st_size}_{int(st.st_mtime)}'  # cache key: changes if CatGT is re-run

        # sync pulse edges (s) in each stream's own clock, same pulse k in both
        self._ni_edges = np.loadtxt(run / f'{name}_tcat.nidq.xd_2_0_500.txt')
        self._im_edges = np.loadtxt(imec / f'{name}_tcat.imec0.ap.xd_384_6_500.txt')
        assert len(self._ni_edges) == len(self._im_edges), 'sync edge counts differ'
        self._offset = self._ni_edges - self._im_edges

    # ---- spikes ----
    def imec_to_ni(self, t_imec):
        """imec-clock seconds -> NI-clock seconds (piecewise-linear through the sync pulses, like TPrime)."""
        return t_imec + np.interp(t_imec, self._im_edges, self._offset)

    def units(self, good_only=True, max_rate=50):
        """Kilosort clusters as a dict of equal-length arrays: cluster_id, KSLabel, n_spikes, firing_rate (Hz).

        good_only keeps clusters labelled 'good' whose mean rate is at most `max_rate` Hz (None = no rate cutoff),
        which drops noise-like clusters. good_only=False returns every cluster.
        """
        with open(self.ks / 'cluster_KSLabel.tsv', newline='') as fh:
            rows = list(csv.DictReader(fh, delimiter='\t'))
        ids = np.array([int(r['cluster_id']) for r in rows], dtype=int)
        label = np.array([r['KSLabel'] for r in rows])
        n = np.bincount(np.load(self.ks / 'spike_clusters.npy'))
        n_spikes = n[ids] if len(ids) else np.zeros(0, int)
        u = {'cluster_id': ids, 'KSLabel': label, 'n_spikes': n_spikes, 'firing_rate': n_spikes / self.duration}
        if not good_only:
            return u
        keep = label == 'good'
        if max_rate is not None:
            keep &= u['firing_rate'] <= max_rate
        return {k: v[keep] for k, v in u.items()}

    def spike_times(self, unit):
        """all spike times (s, NI clock) of one Kilosort cluster."""
        clu = np.load(self.ks / 'spike_clusters.npy', mmap_mode='r')
        t = np.load(self.ks / 'spike_times_sec.npy', mmap_mode='r')  # imec samples / imec rate
        return self.imec_to_ni(np.asarray(t[clu == unit]))

    def spikes_by_unit(self, units):
        """{cluster id: spike times (s, NI clock)} for many units in one pass over the spike table."""
        clu = np.load(self.ks / 'spike_clusters.npy')
        t = np.load(self.ks / 'spike_times_sec.npy', mmap_mode='r')
        order = np.argsort(clu, kind='stable')  # stable keeps each unit's spikes time-sorted
        ids, first = np.unique(clu[order], return_index=True)
        bounds = dict(zip(ids, zip(first, np.r_[first[1:], len(clu)])))
        return {int(u): self.imec_to_ni(np.asarray(t[order[a:b]])) for u in units if u in bounds for a, b in [bounds[u]]}

    def _unit_templates(self):
        """most common Kilosort template among each cluster's spikes (cluster id -> template id). Cached."""
        if not hasattr(self, '_unit_tmpl'):
            clu = np.load(self.ks / 'spike_clusters.npy').astype(np.int64)
            n_tmpl = np.load(self.ks / 'templates.npy', mmap_mode='r').shape[0]
            key = clu * n_tmpl + np.load(self.ks / 'spike_templates.npy')
            self._unit_tmpl = np.bincount(key, minlength=(clu.max() + 1) * n_tmpl).reshape(-1, n_tmpl).argmax(1)
        return self._unit_tmpl

    def _unit_template(self, unit):
        """the unit's most common Kilosort template, (samples, channels)."""
        templates = np.load(self.ks / 'templates.npy', mmap_mode='r')
        return np.asarray(templates[self._unit_templates()[unit]])

    def unit_waveform(self, unit):
        """Kilosort template of the unit's most common template, on its peak channel -> (time ms from the
        largest deflection, waveform). A template, not the median of raw spikes, so it is smoother."""
        tmpl = self._unit_template(unit)
        wf = tmpl[:, np.abs(tmpl).max(0).argmax()]
        return (np.arange(len(wf)) - np.abs(wf).argmax()) / self.imec_fs * 1e3, wf

    def unit_footprint(self, unit):
        """where the unit sits on the probe -> (xy, shank, amp, peak): channel positions (um, channels x 2), shank
        index of each channel, the template's peak-to-peak amplitude on each channel (a.u.) and the peak channel."""
        tmpl = self._unit_template(unit)
        xy = np.load(self.ks / 'channel_positions.npy')
        f = self.ks / 'channel_shanks.npy'
        shank = np.load(f) if f.exists() else np.zeros(len(xy), int)
        amp = tmpl.max(0) - tmpl.min(0)
        return xy, shank, amp, int(amp.argmax())

    # ---- syllables ----
    def label_events(self):
        """(time s, label) of every 0->nonzero event on the 3-bit label port. Cached."""
        f = self.cache / f'{self._tag}_labels.npz'
        if not f.exists():
            w = np.empty(len(self.ni), np.uint8)
            for a in range(0, len(w), 50_000_000):
                w[a:a + 50_000_000] = np.asarray(self.ni[a:a + 50_000_000, 2]).view(np.uint16) & 0xFF
            port = sum(((w >> b) & 1).astype(np.int8) << i for i, b in enumerate(LABEL_LINES))
            ev = np.flatnonzero((port[1:] != 0) & (port[:-1] == 0)) + 1
            # max over the next 3 samples, as snakeSort's decode_port_labels (settle_samples=2)
            lab = np.array([port[e:e + 3].max() for e in ev])
            np.savez(f, t=ev / self.ni_fs, label=lab)
        d = np.load(f)
        return d['t'], d['label']

    def sound_pulses(self):
        """rising/falling edge times (s) of the digital line that is high while the mic is over threshold. Cached."""
        f = self.cache / f'{self._tag}_line{SEG_LINE}_edges.npz'
        if not f.exists():
            x = np.empty(len(self.ni), np.int8)
            for a in range(0, len(x), 50_000_000):
                x[a:a + 50_000_000] = (np.asarray(self.ni[a:a + 50_000_000, 2]).view(np.uint16) >> SEG_LINE) & 1
            d = np.diff(x)
            rise, fall = np.flatnonzero(d == 1) + 1, np.flatnonzero(d == -1) + 1
            if len(fall) and len(rise) and fall[0] < rise[0]:  # high at file start
                fall = fall[1:]
            n = min(len(rise), len(fall))  # drop a pulse still high at file end
            np.savez(f, rise=rise[:n] / self.ni_fs, fall=fall[:n] / self.ni_fs)
        d = np.load(f)
        return d['rise'], d['fall']

    def syllables(self, labeled_only=True, max_lead_ms=5):
        """one entry per syllable, as a dict of arrays: label, pulse, start, end (s). start/end = rise/fall of the sound pulse.

        Each label pulse fires ~1 ms before the rising edge of its sound pulse, so a label is matched to the first
        rise within `max_lead_ms` after it. With labeled_only=False, sound pulses without a label are included
        (label 0, pulse NaN).
        """
        rise, fall = self.sound_pulses()
        t_pulse, label = self.label_events()
        k = np.searchsorted(rise, t_pulse)
        ok = k < len(rise)
        ok[ok] = (rise[k[ok]] - t_pulse[ok]) * 1e3 <= max_lead_ms
        d = {'label': label[ok], 'pulse': t_pulse[ok], 'start': rise[k[ok]], 'end': fall[k[ok]]}
        if not labeled_only:
            rest = np.setdiff1d(np.arange(len(rise)), k[ok])
            d = {'label': np.r_[d['label'], np.zeros(len(rest), int)], 'pulse': np.r_[d['pulse'], np.full(len(rest), np.nan)],
                 'start': np.r_[d['start'], rise[rest]], 'end': np.r_[d['end'], fall[rest]]}
            o = np.argsort(d['start'], kind='stable')
            d = {key: v[o] for key, v in d.items()}
        return d

    # ---- audio ----
    def audio(self, t0, t1):
        """mic signal (V) between t0 and t1 seconds -> (samples, sample times s)."""
        s0, s1 = max(int(t0 * self.ni_fs), 0), min(int(t1 * self.ni_fs), len(self.ni))
        x = np.asarray(self.ni[s0:s1, MIC_CHAN], dtype=np.float32) * self.volts_per_bit
        return x, np.arange(s0, s1) / self.ni_fs

    def spectrogram(self, t0, t1, win=0.008, hop=0.001, fmin=300, fmax=9000):
        """log-power spectrogram (dB) of the mic between t0 and t1 -> (freqs Hz, times s from t0, dB[freq, time])."""
        x, _ = self.audio(t0, t1)
        nper = int(win * self.ni_fs)
        f, t, p = spectrogram(x, self.ni_fs, nperseg=nper, noverlap=nper - int(hop * self.ni_fs), nfft=512)
        band = (f >= fmin) & (f <= fmax)
        return f[band], t, 10 * np.log10(p[band] + 1e-12)

    def mean_spectrogram(self, starts, ends, shape=(256, 256), fmin=300, fmax=9000, **kw):
        """Spectrogram of each syllable from start to end, linearly resampled to `shape` (freq, time), averaged in dB.

        Returns (freqs Hz, time as fraction of syllable duration 0..1, dB[freq, time]); syllables too short for
        two analysis windows are skipped.
        """
        out = []
        for s0, s1 in zip(starts, ends):
            if s1 - s0 < 2 * kw.get('win', 0.008):  # need at least 2 frames
                continue
            f, t, p = self.spectrogram(s0, s1, fmin=fmin, fmax=fmax, **kw)
            if p.shape[1] < 2:
                continue
            p = interp1d(f, p, axis=0)(np.linspace(f[0], f[-1], shape[0]))
            out.append(interp1d(t, p, axis=1)(np.linspace(t[0], t[-1], shape[1])))
        return np.linspace(fmin, fmax, shape[0]), np.linspace(0, 1, shape[1]), np.mean(out, axis=0)
