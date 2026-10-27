import os, re, sys
from PIL import Image, ImageDraw
# usage: strip.py <dumpdir> <steps file> <out.png> <crop x0,y0,x1,y1> <scale> label...   (labels side by side, no diff)
d, steps, out, c, scale = sys.argv[1], sys.argv[2], sys.argv[3], tuple(int(v) for v in sys.argv[4].split(',')), float(sys.argv[5])
order = re.findall(r"\{ '(\w+)',", open(os.path.join(os.path.dirname(os.path.abspath(__file__)), steps)).read())
names = sorted(f for f in os.listdir(d) if f.endswith('.bmp'))
last = {}
for n in names:
    r = Image.open(os.path.join(d, n)).convert('RGB').getpixel((3, 3))
    if r[1] > 200:
        last[1 + round(r[0] / 33) + 8 * round(r[2] / 33)] = n
cells = [Image.open(os.path.join(d, last[order.index(l) + 1])).convert('RGB').crop(c) for l in sys.argv[6:]]
w, h = int((c[2] - c[0]) * scale), int((c[3] - c[1]) * scale)
sheet = Image.new('RGB', (w * len(cells), h + 18), (20, 20, 20))
dr = ImageDraw.Draw(sheet)
for k, (im, l) in enumerate(zip(cells, sys.argv[6:])):
    sheet.paste(im.resize((w, h), Image.LANCZOS), (k * w, 18)); dr.text((k * w + 4, 3), l, fill=(255, 255, 255))
sheet.save(out); print(out, sheet.size)
