# perf.py <log> [--merge]: averages the per-pass GPU ms of the render profile blocks (FAF_GPU_PROFILE) per mode of pp_camera.lua; the first two
# blocks after a switch are dropped. With --merge, repeated modes (OFF twice) are averaged together.
import sys, re, collections
text = open(sys.argv[1], encoding='utf-8', errors='ignore').read().replace('\0', '')
mode = None; skip = 0; cur = None; seg = 0
data = collections.defaultdict(lambda: collections.defaultdict(list))
tot = collections.defaultdict(list)
for line in text.splitlines():
    m = re.search(r'PP-TEST: mode (.+?) aa (\d) sharpen (\d) wall', line)
    if m:
        name = m.group(1)
        mode = name if '--merge' in sys.argv else f'{name}#{seg}'
        seg += 1; skip = 2; cur = None
        continue
    if 'Render profile over' in line:
        if mode is None: continue
        if skip > 0: skip -= 1; cur = None; continue
        cur = mode
        g = re.search(r'GPU ([\d.]+) ms/frame', line); tot[cur].append(float(g.group(1)))
        continue
    m = re.match(r'info:\s+pass (\w+)\s+[\d.]+ calls\s+CPU\s+([\d.]+) ms\s+GPU\s+([\d.]+) ms', line)
    if m and cur:
        data[cur][m.group(1)].append(float(m.group(3)))
names = ['AntiAliasing', 'Sharpen', 'Terrain', 'Meshes', 'Effects']
print('mode                      n  GPUtotal  ' + '  '.join(f'{n:>12}' for n in names))
for k in tot:
    n = len(tot[k]); avg = lambda l: sum(l) / len(l) if l else 0
    print(f'{k:24} {n:3}  {avg(tot[k]):8.3f}  ' + '  '.join(f'{avg(data[k][nm]):12.4f}' for nm in names))
