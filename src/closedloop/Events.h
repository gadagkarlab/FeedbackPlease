#ifndef CLOSEDLOOP_EVENTS_H
#define CLOSEDLOOP_EVENTS_H

// The two event types that cross thread boundaries via ThreadSafeQueue.
//
// All cross-stream timing is done in IMEC sample indices: NiFetchThread
// maps each syllable onset from the NI stream into the IMEC stream via
// sglx_mapSample (SpikeGLX's own sync-pulse-based stream alignment), so
// DecisionThread can compute "spike time relative to syllable onset" as a
// plain difference of two IMEC sample indices -- see README.md's
// "Cross-stream time alignment".

struct SpikeEvent {
    long long  sampleIndex;      // absolute IMEC stream sample index
};

struct SyllableEvent {
    int        code;             // 1-7 (debounce-confirmed 3-bit code from NI lines 5/6/7)
    long long  sampleIndex;      // absolute NI stream sample index (debounce-confirmed onset)
    long long  imecSampleIndex;  // same onset, mapped into the IMEC stream via sglx_mapSample
};

#endif // CLOSEDLOOP_EVENTS_H
