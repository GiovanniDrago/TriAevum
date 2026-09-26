"""Summarize steady-state ABBA AOT runs; reject incomplete/mismatched workloads."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(rows):
    if not rows or len(rows) % 4:
        raise ValueError('Expected complete ABBA groups')
    if len({r['memory_fingerprint'] for r in rows}) != 1:
        raise ValueError('Final game memory differs')
    if any(r.get('intrusive_native_sampling') for r in rows):
        raise ValueError('Sampled runs are not performance evidence')
    pairs = [(a, b) for i in range(0, len(rows), 4) for a, b in ((i, i + 1), (i + 3, i + 2))]
    frames = {r['benchmark']['measured_frames'] for r in rows}
    if len(frames) != 1:
        raise ValueError('Different steady-state window lengths')
    result = {'pairs': pairs, 'fingerprint': rows[0]['memory_fingerprint'], 'measured_updates_per_run': frames.pop()}
    getters = {
        'pre_backend_envelope_ms': lambda b: b['pre_backend_cpu']['host_envelope_mean_ms'],
        'accounted_pre_backend_ms': lambda b: b['pre_backend_cpu']['mean_ms'],
        'guest_ms': lambda b: b['pre_backend_cpu']['guest_seconds'] * 1000 / b['measured_frames'],
        'guest_excluding_clock_ms': lambda b: (b['pre_backend_cpu']['guest_seconds'] -
            b['pre_backend_cpu']['guest_clock_included_seconds']) * 1000 / b['measured_frames'],
        'whole_frame_ms': lambda b: b['frame_times']['mean_ms'],
        'pre_backend_p95_ms': lambda b: b['pre_backend_cpu']['host_envelope_p95_upper_ms'],
    }
    for name, getter in getters.items():
        values = [getter(r['benchmark']) for r in rows]
        ratios = [values[b] / values[a] for a, b in pairs]
        result[name] = {
            'control_median': statistics.median(values[a] for a, _ in pairs),
            'candidate_median': statistics.median(values[b] for _, b in pairs),
            'paired_ratios': ratios,
            'paired_median_ratio': statistics.median(ratios),
        }
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('measurements', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = json.dumps(summarize(json.loads(args.measurements.read_text())), indent=2)
    if args.output:
        args.output.write_text(result)
    print(result)
