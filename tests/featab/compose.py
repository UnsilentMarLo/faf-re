import sys, os
from PIL import Image, ImageDraw, ImageChops, ImageStat
# usage: compose.py <dumpdir> <outprefix> <crop x0,y0,x1,y1 | full> <name:labelA,labelB[,labelC...]> ...
# labels are step labels of steps_static.lua; the frame of a step is the last dumped frame whose corner marker carries the step's number
# (red = 33 * ((n - 1) mod 8), blue = 33 * ((n - 1) div 8), written by featab_camera.lua).  row = A, B, ..., |A-B| x10
import re
here = os.path.dirname(os.path.abspath(__file__))
labels_in_order = re.findall(r"\{ '(\w+)',", open(os.path.join(here, os.environ.get('FEATAB_STEPS', 'steps_static.lua'))).read())
d = sys.argv[1]
names = sorted(f for f in os.listdir(d) if f.endswith('.bmp'))
last = {}
for n in names:
    r = Image.open(os.path.join(d, n)).convert('RGB').getpixel((3, 3))
    if r[1] > 200:
        last[1 + round(r[0] / 33) + 8 * round(r[2] / 33)] = n
def frame(label):
    i = labels_in_order.index(label) + 1
    return Image.open(os.path.join(d, last[i])).convert('RGB')
def crop(img, c):
    return img if c == 'full' else img.crop(tuple(int(v) for v in c.split(',')))
def stats(a, b):
    diff = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(diff).mean) / 3
    mx = diff.convert('L').point(lambda v: 255 if v > 8 else 0)
    return diff, mean, ImageStat.Stat(mx).mean[0] / 255 * 100
out = sys.argv[2]; c = sys.argv[3]
for spec in sys.argv[4:]:
    name, labels = spec.split(':'); labels = labels.split(',')
    ims = [crop(frame(l), c) for l in labels]
    cells = list(ims)
    for l, im in zip(labels[1:], ims[1:]):
        diff, mean, frac = stats(ims[0], im)
        print('%-14s %s vs %s: mean abs diff %.3f, pixels >8 levels %.2f %%' % (name, l, labels[0], mean, frac))
    diff, _, _ = stats(ims[0], ims[1])
    cells.append(diff.point(lambda v: min(255, v * 10)))
    w, h = cells[0].size
    sheet = Image.new('RGB', (w * len(cells), h + 18), (20, 20, 20))
    dr = ImageDraw.Draw(sheet)
    for k, im in enumerate(cells):
        sheet.paste(im, (k * w, 18))
        dr.text((k * w + 4, 3), (labels[k] if k < len(labels) else '|diff 1-2| x10'), fill=(255, 255, 255))
    p = f'{out}_{name}.png'; sheet.save(p); print(p, sheet.size)
