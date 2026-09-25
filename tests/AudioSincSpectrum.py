"""Measure actual 48 kHz PCM; exits nonzero when the requested sinc gate fails.

Only Python's standard library is needed. Hann-weighted tone projections avoid
FFT-bin rounding; the frequencies come from the DS timer and source loop size.
Passband is relative to the 0.1-Nyquist reference. Images include both source
reconstruction and the existing mixer-to-blip stage, without masking the latter.
"""
import argparse
from array import array
import json
import math
from pathlib import Path
import re

CLOCK = 16756991.0
RATE = 48000.0
MIX = CLOCK/512


def amplitude(samples, frequency):
    # Recurrence is substantially cheaper than evaluating sin/cos per frame.
    step = complex(math.cos(2*math.pi*frequency/RATE), math.sin(2*math.pi*frequency/RATE))
    rotor = 1+0j
    total = 0j
    for sample in samples:
        total += sample*rotor
        rotor *= step
    return 2*abs(total)


def db(value):
    return 20*math.log10(max(value, 1e-12))


def measure(root):
    rows = []
    for path in sorted(root.glob('m*-p*-b*-*.pcm')):
        match = re.fullmatch(r'm(\d+)-p(\d+)-b(\d+)-(sine|square)\.pcm', path.name)
        if not match:
            continue
        mode, period, bin = map(int, match.groups()[:3])
        waveform = match[4]
        raw = array('h', path.read_bytes())
        samples = raw[12000*2::2]  # Drop 250 ms of startup/filter transient.
        n = len(samples)
        window = [0.5-0.5*math.cos(2*math.pi*i/(n-1)) for i in range(n)]
        scale = sum(window)
        samples = [s*w/scale for s, w in zip(samples, window)]
        source = CLOCK/period
        nyquist = min(source, MIX)/2
        frequency = source*bin/4096
        fundamental = amplitude(samples, frequency)
        images = set()
        for clock in (source, MIX):
            for multiple in range(1, int(24000/clock)+2):
                for sign in (-1, 1):
                    image = multiple*clock+sign*frequency
                    if 30 < image < 23900 and abs(image-frequency) > 30:
                        # Square-wave harmonics are desired content, not spurs.
                        harmonic = image/frequency
                        if waveform == 'square' and image < nyquist and abs(harmonic-round(harmonic)) < 0.01:
                            continue
                        images.add(image)
        strongest = max(((amplitude(samples, f), f) for f in images), default=(0, 0))
        alias = abs((frequency+MIX/2) % MIX-MIX/2)
        alias_level = amplitude(samples, alias) if frequency > nyquist else 0
        rows.append(dict(mode=mode, period=period, bin=bin, wave=waveform,
                         frequency=frequency, fraction=frequency/nyquist, amplitude=fundamental,
                         image_amplitude=strongest[0], image_frequency=strongest[1],
                         image_count=len(images),
                         alias_amplitude=alias_level, samples=n))
    summary = []
    for mode in sorted({r['mode'] for r in rows}):
        for period in sorted({r['period'] for r in rows}):
            group = [r for r in rows if r['mode'] == mode and r['period'] == period]
            if not group:
                continue
            ref = min((r for r in group if r['wave'] == 'sine'), key=lambda r:r['frequency'])['amplitude']
            band = [r for r in group if r['wave'] == 'sine' and r['fraction'] <= 0.851]
            gains = [db(r['amplitude']/ref) for r in band]
            images = [db(r['amplitude']/max(r['image_amplitude'],1e-12)) for r in band if r['image_count']]
            alias = [db(ref/max(r['alias_amplitude'],1e-12)) for r in group if r['fraction'] >= 1.049]
            square = [db(r['amplitude']/max(r['image_amplitude'],1e-12)) for r in group if r['wave']=='square' and r['image_count']]
            summary.append(dict(mode=mode, period=period, pass_min_db=min(gains), pass_max_db=max(gains),
                                image_rejection_db=min(images), alias_rejection_db=min(alias) if alias else None,
                                square_image_rejection_db=min(square)))
    return dict(rows=rows, summary=summary)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('pcm', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = measure(args.pcm)
    report['sinc_gate_pass'] = all(r['pass_min_db'] >= -0.5 and r['pass_max_db'] <= 0.5 and
                                 r['image_rejection_db'] >= 60 and r['square_image_rejection_db'] >= 60 and
                                 (r['alias_rejection_db'] is None or r['alias_rejection_db'] >= 60)
                                 for r in report['summary'] if r['mode'] == 6) and any(r['mode']==6 for r in report['summary'])
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    for row in report['summary']:
        print(row)
    print('Sinc quality gate:', 'PASS' if report['sinc_gate_pass'] else 'FAIL')
    return 0 if report['sinc_gate_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
