import copy
import unittest
from summarize_aot_abba import summarize


def rows():
    out = []
    for duration in (10, 8, 8, 10):
        out.append({'memory_fingerprint': 7, 'benchmark': {
            'measured_frames': 1000, 'frame_times': {'mean_ms': duration},
            'pre_backend_cpu': {'host_envelope_mean_ms': duration, 'mean_ms': duration,
                'guest_seconds': duration, 'guest_clock_included_seconds': 0,
                'host_envelope_p95_upper_ms': duration + 1}}})
    return out


class AbbaTests(unittest.TestCase):
    def test_pairs_both_directions(self):
        summary = summarize(rows())
        self.assertEqual(summary['pre_backend_envelope_ms']['paired_ratios'], [0.8, 0.8])

    def test_rejects_mismatched_or_sampled_workloads(self):
        changes = [lambda r: r.pop(), lambda r: r[1].update(memory_fingerprint=8),
            lambda r: r[2].update(intrusive_native_sampling=True),
            lambda r: r[3]['benchmark'].update(measured_frames=999)]
        for change in changes:
            data = copy.deepcopy(rows())
            change(data)
            with self.assertRaises(ValueError):
                summarize(data)


if __name__ == '__main__':
    unittest.main()
