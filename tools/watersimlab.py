#!/usr/bin/env python3
"""Driver and measurements of the headless water lab (tools/watersimlab, docs/LIGHTING_V2.md "Water lab").

  watersimlab.py run      [--terrain coast] [--seconds 40] [--from 20] [--every 4] [--out DIR] [--src FILE]
                          runs the real simulation shader of the game on the CPU (no game, no GPU) and writes captures + look images
  watersimlab.py metrics  DIR      numbers of the last captures: crest alignment with the shore, foam on the shore, limits
  watersimlab.py selftest [--terrain coast]
                          runs a lab simulation and checks the numbers against what a sea should look like (exit 1 when one fails)
  watersimlab.py strip    DIR [-o out.png] [--crop x0,y0,x1,y1]   the look images of a run, side by side

The captures are the format of the in-game recorder (tools/waterrec.py reads them too).
"""
import argparse
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import waterrec  # noqa: E402

EXE = os.path.join(HERE, 'watersimlab', 'watersimlab.exe')
SRC = os.path.join(HERE, '..', 'src', 'moho', 'terrain', 'water', 'WaterSimulation.cpp')
OUT = os.path.join(HERE, '..', '..', 'faf-re-integ', 'out', 'lab')


def run(terrain='coast', seconds=40.0, start=20.0, every=4.0, out=OUT, src=SRC, look=3, extra=()):
    if not os.path.exists(EXE):
        sys.exit('build the lab first: tools/watersimlab/build.ps1')
    cmd = [EXE, '--src', src, '--out', out, '--terrain', terrain, '--seconds', str(seconds), '--from', str(start), '--every', str(every), '--look', str(look)]
    cmd += list(extra)
    subprocess.run(cmd, check=True)
    return out


def distance_to_land(land, n):
    """Chamfer distance (cells) to the nearest land cell: two passes, 3-4 weights."""
    inf = 1e9
    d = [0.0 if land[i] else inf for i in range(n * n)]
    for j in range(n):
        for i in range(n):
            k = j * n + i
            v = d[k]
            if i > 0: v = min(v, d[k - 1] + 1.0)
            if j > 0:
                v = min(v, d[k - n] + 1.0)
                if i > 0: v = min(v, d[k - n - 1] + 1.4142)
                if i < n - 1: v = min(v, d[k - n + 1] + 1.4142)
            d[k] = v
    for j in range(n - 1, -1, -1):
        for i in range(n - 1, -1, -1):
            k = j * n + i
            v = d[k]
            if i < n - 1: v = min(v, d[k + 1] + 1.0)
            if j < n - 1:
                v = min(v, d[k + n] + 1.0)
                if i < n - 1: v = min(v, d[k + n + 1] + 1.4142)
                if i > 0: v = min(v, d[k + n - 1] + 1.4142)
            d[k] = v
    return d


def smooth(field, n, radius):
    """Box blur (both directions) of a float list."""
    for axis in (0, 1):
        out = [0.0] * (n * n)
        for j in range(n):
            for i in range(n):
                s = 0.0
                c = 0
                for o in range(-radius, radius + 1):
                    ii = i + o if axis == 0 else i
                    jj = j + o if axis == 1 else j
                    if 0 <= ii < n and 0 <= jj < n:
                        s += field[jj * n + ii]
                        c += 1
                out[j * n + i] = s / c
        field = out
    return field


def measure(rec):
    n = rec['crop']
    cell = rec['cell']
    land = rec['land']
    h = rec['height']
    foam = rec['foam']
    dist = smooth(distance_to_land(land, n), n, 3)          # cells, smoothed: the shore normal is its gradient
    hs = smooth([h[i] for i in range(n * n)], n, 1)
    # alignment of the crests with the shore: the gradient of the height against the gradient of the distance
    sum_c = sum_s = weight = 0.0
    band = 0
    for j in range(4, n - 4):
        for i in range(4, n - 4):
            k = j * n + i
            d = dist[k] * cell
            if land[k] or d < 8.0 or d > 60.0:
                continue
            gx = hs[k + 1] - hs[k - 1]
            gz = hs[k + n] - hs[k - n]
            hx = dist[k + 1] - dist[k - 1]
            hz = dist[k + n] - dist[k - n]
            gm = math.hypot(gx, gz)
            hm = math.hypot(hx, hz)
            if gm < 1e-4 or hm < 1e-3:
                continue
            # angle between the crest normal and the shore normal, doubled (a crest normal has no sign)
            cosd = (gx * hx + gz * hz) / (gm * hm)
            sind = (gx * hz - gz * hx) / (gm * hm)
            a2 = 2.0 * math.atan2(sind, cosd)
            sum_c += gm * math.cos(a2)
            sum_s += gm * math.sin(a2)
            weight += gm
            band += 1
    r = math.hypot(sum_c, sum_s) / weight if weight > 0 else float('nan')   # 1 = every crest parallel to the shore, 0 = every direction
    # foam on the shore strip (water cells within 6 units of land)
    strip = covered = 0
    foam_max = 0.0
    for k in range(n * n):
        if land[k]:
            continue
        d = dist[k] * cell
        if d <= 6.0:
            strip += 1
            if foam[k] > 0.5:
                covered += 1
        foam_max = max(foam_max, foam[k])
    finite = all(math.isfinite(v) for v in h) and all(math.isfinite(v) for v in foam)
    return dict(time=rec['time'], alignment=r, strip_foam=(covered / strip if strip else 0.0), foam_max=foam_max, h_min=min(h), h_max=max(h), finite=finite, cells=band)


def cmd_metrics(folder):
    recs = waterrec.load(folder)
    if not recs:
        sys.exit('no captures in ' + folder)
    rows = [measure(r) for r in recs[-3:]]
    print('time   crest alignment (1 parallel .. 0 any)   shore strip foam   foam max   h min   h max')
    for m in rows:
        print('%5.1f  %.3f                                 %.3f              %.2f       %.3f  %.3f%s' % (m['time'], m['alignment'], m['strip_foam'], m['foam_max'], m['h_min'], m['h_max'], '' if m['finite'] else '  NOT FINITE'))
    return rows


def cmd_selftest(terrain):
    out = run(terrain=terrain, seconds=40.0, start=32.0, every=4.0, out=OUT + '_selftest', look=2)
    rows = [measure(r) for r in waterrec.load(out)]
    failures = []
    for m in rows:
        if not m['finite']:
            failures.append('not finite at t=%.1f' % m['time'])
        if not (m['h_min'] > -0.5 and m['h_max'] < 0.6):
            failures.append('height out of range at t=%.1f (%.3f .. %.3f)' % (m['time'], m['h_min'], m['h_max']))
        if not (m['alignment'] < 0.80):
            failures.append('crests too aligned with the shore at t=%.1f (%.3f, want < 0.80: a real sea is noisier)' % (m['time'], m['alignment']))
        if not (0.02 < m['strip_foam'] < 0.75):
            failures.append('shore foam coverage %.3f at t=%.1f outside 0.02 .. 0.75' % (m['strip_foam'], m['time']))
    for m in rows:
        print('t=%5.1f alignment %.3f strip foam %.3f foam max %.2f height %.3f .. %.3f' % (m['time'], m['alignment'], m['strip_foam'], m['foam_max'], m['h_min'], m['h_max']))
    if failures:
        print('FAIL')
        for f in failures:
            print('  ' + f)
        return 1
    print('PASS')
    return 0


def cmd_strip(folder, out, crop):
    from PIL import Image
    files = sorted(f for f in os.listdir(folder) if f.startswith('look_') and f.endswith('.bmp'))
    ims = []
    for f in files[-4:]:
        im = Image.open(os.path.join(folder, f)).convert('RGB')
        if crop:
            im = im.crop(crop)
        ims.append(im)
    w, h = ims[0].size
    sheet = Image.new('RGB', (w * len(ims), h))
    for k, im in enumerate(ims):
        sheet.paste(im, (k * w, 0))
    sheet.save(out)
    print('wrote', out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--terrain', default='coast')
    r.add_argument('--seconds', type=float, default=40.0)
    r.add_argument('--from', dest='start', type=float, default=20.0)
    r.add_argument('--every', type=float, default=4.0)
    r.add_argument('--out', default=OUT)
    r.add_argument('--src', default=SRC)
    m = sub.add_parser('metrics')
    m.add_argument('dir')
    t = sub.add_parser('selftest')
    t.add_argument('--terrain', default='coast')
    s = sub.add_parser('strip')
    s.add_argument('dir')
    s.add_argument('-o', default='strip.png')
    s.add_argument('--crop', default='')
    a = ap.parse_args()
    if a.cmd == 'run':
        run(a.terrain, a.seconds, a.start, a.every, a.out, a.src)
    elif a.cmd == 'metrics':
        cmd_metrics(a.dir)
    elif a.cmd == 'selftest':
        sys.exit(cmd_selftest(a.terrain))
    elif a.cmd == 'strip':
        cmd_strip(a.dir, a.o, tuple(int(v) for v in a.crop.split(',')) if a.crop else None)


if __name__ == '__main__':
    main()
