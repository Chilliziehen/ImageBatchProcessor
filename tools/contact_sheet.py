from pathlib import Path
from PIL import Image, ImageOps, ImageDraw
import argparse

p = argparse.ArgumentParser()
p.add_argument('directory', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--pairs', action='store_true')
p.add_argument('--stride', type=int, default=1)
p.add_argument('--page-size', type=int, default=32)
a = p.parse_args()
files = sorted(x for x in a.directory.glob('*.jpg') if not x.name.endswith('.before.jpg'))[::a.stride]
for page in range(0, len(files), a.page_size):
    group = files[page:page+a.page_size]
    columns = 4 if a.pairs else 5
    cells = len(group) * (2 if a.pairs else 1)
    w, h = (360, 270)
    canvas = Image.new('RGB', (columns*w, ((cells+columns-1)//columns)*h), '#171b20')
    draw = ImageDraw.Draw(canvas)
    n = 0
    for f in group:
        items = [(f.with_name(f.stem+'.before.jpg'), 'BEFORE'), (f, 'AFTER')] if a.pairs else [(f, '')]
        for path, label in items:
            with Image.open(path) as im:
                thumb = ImageOps.contain(im.convert('RGB'), (w-10, h-30))
            x, y = (n % columns)*w, (n//columns)*h
            canvas.paste(thumb, (x+(w-thumb.width)//2, y+(h-30-thumb.height)//2))
            draw.text((x+8,y+h-24),f'{f.stem} {label}',fill='white')
            n += 1
    destination = a.output if len(files) <= a.page_size else a.output.with_name(a.output.stem+f'_{page//a.page_size+1:02}'+a.output.suffix)
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(destination, quality=90)
    print(destination)
