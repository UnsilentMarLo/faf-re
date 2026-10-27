# crops.py <frames-prefix> <out.png> <x> <y> <w> <h> <scale> [index ...]: tiles the same crop of the picked frames (pick_frames.py output
# <prefix>_<k>.png) side by side, enlarged with nearest neighbour, for paired comparisons of edges.
import sys
from PIL import Image, ImageDraw
prefix, out, x, y, w, h, scale = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:8])
indices = [int(v) for v in sys.argv[8:]] or [0, 1, 2]
tiles = []
for k in indices:
    im = Image.open(f'{prefix}_{k}.png').convert('RGB').crop((x, y, x + w, y + h)).resize((w * scale, h * scale), Image.NEAREST)
    d = ImageDraw.Draw(im); d.rectangle((0, 0, 60, 14), fill=(0, 0, 0)); d.text((3, 1), f'#{k}', fill=(255, 255, 255))
    tiles.append(im)
sheet = Image.new('RGB', (sum(t.width for t in tiles) + 4 * (len(tiles) - 1), tiles[0].height), (255, 0, 255))
cx = 0
for t in tiles:
    sheet.paste(t, (cx, 0)); cx += t.width + 4
sheet.save(out)
