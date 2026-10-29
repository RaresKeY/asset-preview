#!/usr/bin/env python3
"""Compare retained partial repaints with clean frames at native/fractional scale."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--scales', nargs='+', default=['1', '1.25', '1.5', '2'])
    parser.add_argument('--styles', nargs='+', default=['Fusion', 'Breeze'])
    parser.add_argument('--compositor', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='overview-paint-') as temporary:
        output = Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR', temporary)).resolve()
        output.mkdir(parents=True, exist_ok=True)
        records, failures = [], []
        for style in args.styles:
            for scale in args.scales:
                label = f'{style.lower()}-{scale}'
                target = output / label
                target.mkdir(exist_ok=True)
                environment = {**os.environ, 'QT_SCALE_FACTOR': scale,
                               'ASSET_PREVIEW_TEST_STYLE': style, 'ASSET_PREVIEW_EVIDENCE_DIR': str(target)}
                command = [str(args.binary.resolve())] + (['--compositor'] if args.compositor else [])
                result = subprocess.run(command, env=environment, text=True, capture_output=True, timeout=40)
                (target / 'run.log').write_text(result.stdout + result.stderr)
                if result.returncode:
                    failures.append(label)
                    print(f'{label}: FAIL {result.stderr[-500:]}', flush=True)
                else:
                    record = json.loads(next(line for line in result.stdout.splitlines() if line.startswith('{')))
                    records.append(record)
                    print(f'{label}: {record["interaction_states"]} states, '
                          f'{record["retained_frames"]} retained and {record["compositor_frames"]} compositor frames verified', flush=True)
        (output / 'results.json').write_text(json.dumps({'records': records, 'failures': failures}, indent=2) + '\n')
        return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
