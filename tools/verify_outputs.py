"""Validate batch artifacts, then generate a local, offline photo review gallery."""
from pathlib import Path
from PIL import Image, ImageOps
import argparse, json, zlib, struct, html, hashlib

p=argparse.ArgumentParser()
p.add_argument('workspace',type=Path)
p.add_argument('--partial',action='store_true')
a=p.parse_args()
rows=[];missing=[];errors=[]
for source_dir,outname in [(a.workspace/'Raw','Raw_Out'),(a.workspace,'Current_Out')]:
    output=a.workspace/outname
    thumbs=output/'_review';thumbs.mkdir(exist_ok=True,parents=True)
    for source in sorted(source_dir.glob('*.NEF')):
        image=output/(source.stem+'.jpg')
        recipe=output/(source.stem+'.imgarg')
        analysis=output/(source.stem+'.analysis.json')
        before=output/(source.stem+'.input.json')
        if not all(x.exists() for x in [image,recipe,analysis,before]):
            missing.append(str(source));continue
        try:
            data=recipe.read_bytes()
            assert len(data)==5408 and data[:8]==b'IMGARG\r\n'
            header=struct.unpack('<6I',data[8:32])
            assert header==(1,0x01020304,32,5376,zlib.crc32(data[32:]),1)
            stats=json.loads(analysis.read_text())
            assert all(sum(h)==stats['samples'] for h in stats['histogramYRGB'])
            original=json.loads(before.read_text())
            with Image.open(image) as im:
                im.load()
                assert min(im.size)>=2000
                assert im.info.get('icc_profile'), 'No sRGB ICC profile'
                dims=im.size
                thumb=ImageOps.contain(im,(360,280))
                thumb.save(thumbs/(source.stem+'.jpg'),quality=84)
            flags=[]
            if original['p95']<.035 and original['median']<.012:
                flags.append('近乎全黑原片：提亮后主要为噪声')
            if stats['whiteClip']>.005:
                flags.append('强反光/高光：建议检查')
            rows.append(dict(source=str(source),output=f'{outname}/{image.name}',thumbnail=f'{outname}/_review/{image.name}',name=source.stem,group=outname,width=dims[0],height=dims[1],bytes=image.stat().st_size,exposure=struct.unpack_from('<f',data,32)[0],median=stats['median'],p99=stats['p99'],whiteClip=stats['whiteClip'],flags=flags,sha256=hashlib.sha256(image.read_bytes()).hexdigest()))
        except Exception as e:
            errors.append(dict(file=str(image),error=str(e)))
    print(outname, 'checked',sum(r['group']==outname for r in rows),flush=True)
summary=dict(count=len(rows),missing=missing,errors=errors,flagged=[r['output'] for r in rows if r['flags']],bytes=sum(r['bytes'] for r in rows),files=rows)
(a.workspace/'processing_report.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
cards=[]
for r in rows:
    e=html.escape
    cards.append(f'''<a class="card" data-group="{r['group']}" data-flag="{bool(r['flags'])}" href="{e(r['output'])}" target="_blank"><img loading="lazy" src="{e(r['thumbnail'])}" alt="{r['name']}"><div><b>{r['name']}</b><span>{r['group']} · {r['width']} × {r['height']}</span><span>曝光 {r['exposure']:+.2f} EV · 亮度 {r['median']:.2f}</span><small>{e('；'.join(r['flags']))}</small></div></a>''')
page='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>照片处理结果</title><style>
*{box-sizing:border-box}body{background:#10151c;color:#e5eaf1;font:15px system-ui,sans-serif;margin:0}header{padding:36px 5vw 22px;position:sticky;top:0;background:#10151cf5;z-index:1;border-bottom:1px solid #303844}h1{font-size:30px;margin:0 0 12px}p{color:#a9b7c8;line-height:1.7;max-width:1000px}select,input{background:#202b38;border:1px solid #526272;color:white;border-radius:6px;padding:10px;margin-right:8px}main{padding:28px 5vw;display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:20px}.card{display:block;background:#1a222d;border-radius:9px;overflow:hidden;text-decoration:none;color:inherit}.card img{width:100%;height:255px;object-fit:contain;background:#0a0f15}.card div{padding:14px}.card b,.card span{display:block}.card span{font-size:12px;color:#9faec0;margin-top:6px}.card small{color:#e6b57c;line-height:1.7}a:hover{outline:1px solid #70b3cb}</style><header><h1>ImageBatchProcessor · 照片处理结果</h1><p>COUNT 张全分辨率 sRGB JPEG，质量 95。原 RAW 保留；配方与分析数据在对应输出目录。自然亮度、柔和高光与适度阴影提升。部分原片近乎全黑或已失焦，仍保留输出；此类缺失细节不能由曝光和锐化恢复。</p><select id="group"><option value="">全部照片</option><option>Raw_Out</option><option>Current_Out</option></select><input id="search" placeholder="搜索文件名"><label><input id="flag" type="checkbox">仅看需要留意的照片</label></header><main>CARDS</main><script>function filter(){document.querySelectorAll('.card').forEach(c=>{c.style.display=(!group.value||c.dataset.group===group.value)&&c.textContent.toLowerCase().includes(search.value.toLowerCase())&&(!flag.checked||c.dataset.flag==='True')?'':'none'})}group.onchange=search.oninput=flag.onchange=filter;</script></html>'''
(a.workspace/'修图报告.html').write_text(page.replace('COUNT',str(len(rows))).replace('CARDS',''.join(cards)),encoding='utf-8')
print(json.dumps({k:v for k,v in summary.items() if k!='files'},ensure_ascii=False))
if errors or (missing and not a.partial):raise SystemExit(1)
