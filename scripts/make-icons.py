"""Export a colored horse/monitor app icon and purpose-drawn monochrome tray mark."""
from pathlib import Path
from PIL import Image, ImageDraw
import argparse, io, json, shutil, struct
import xml.etree.ElementTree as ET
import resvg_py

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'src'
SVG='{http://www.w3.org/2000/svg}'
ET.register_namespace('',SVG[1:-1])
APP_SIZES=[16,20,24,28,32,40,48,64,128,256]
TRAY_SIZES=[16,20,24,28,32,40,48]
REVISION='2026-10-12-free-app-icon-design-v4'

def vector(kind,size=None):
    source='app-icon.svg' if kind=='app' else 'tray-mark.svg'
    root=ET.fromstring((OUT/source).read_text(encoding='utf8'))
    root.set('width',str(size or 512));root.set('height',str(size or 512))
    if kind=='app' and size and size<=32:
        detail_ids={'ear-detail','cheek','nostril','mane-detail'}
        if size<=20:detail_ids.add('eye')
        for parent in root.iter():
            for child in list(parent):
                if child.get('id') in detail_ids:parent.remove(child)
        if size<=24:
            rays=root.find('.//'+SVG+"path[@id='sun-rays']")
            rays.set('d','M355 202V194M355 276V284M318 239H310M392 239H400')
            rays.set('stroke-width','8')
    if kind!='app':
        root.find(SVG+'g').set('fill','#FFFFFF' if kind=='tray-dark' else '#111111')
    return ET.tostring(root,encoding='unicode')

def pixels(kind,size):
    return resvg_py.svg_to_bytes(svg_string=vector(kind,size),width=size,height=size,skip_system_fonts=True)

def ico(kind,sizes):
    data=[pixels(kind,size) for size in sizes]
    offset=6+16*len(sizes);entries=[]
    for size,png in zip(sizes,data):
        entries.append(struct.pack('<BBBBHHII',size%256,size%256,0,0,1,32,len(png),offset));offset+=len(png)
    (OUT/(('app' if kind=='app' else kind)+'.ico')).write_bytes(struct.pack('<HHH',0,1,len(sizes))+b''.join(entries)+b''.join(data))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--export-directory',type=Path);args=parser.parse_args()
    (ROOT/'build').mkdir(exist_ok=True)
    for kind,sizes in [('app',APP_SIZES),('tray-dark',TRAY_SIZES),('tray-light',TRAY_SIZES)]:
        ico(kind,sizes)
        stem='app-icon' if kind=='app' else kind
        (OUT/(stem+'.png')).write_bytes(pixels(kind,512))
        if kind!='app':(OUT/(stem+'.svg')).write_text(vector(kind)+'\n',encoding='utf8')
    preview=Image.new('RGB',(960,420),'#F3F5F7');draw=ImageDraw.Draw(preview)
    draw.text((28,26),'XIAOMA BRIGHTNESS / V4',fill='#213846')
    app=Image.open(io.BytesIO(pixels('app',240))).convert('RGBA');preview.paste(app,(20,80),app)
    draw.text((28,344),'HORSE / DISPLAY / SUN',fill='#213846')
    draw.text((28,368),'Editable vector + native ICO',fill='#526675')
    panels=[('app',48,132,'#202932','#B4C4CC','App / dark'),('app',142,226,'#FFFFFF','#526675','App / light'),
            ('tray-dark',244,308,'#202932','#B4C4CC','Tray / dark'),('tray-light',320,384,'#FFFFFF','#526675','Tray / light')]
    for kind,top,bottom,bg,ink,label in panels:
        draw.rounded_rectangle((282,top,932,bottom),radius=8,fill=bg)
        draw.text((298,top+10),label,fill=ink)
        for size,x in [(16,426),(20,510),(24,594),(32,678),(48,778),(64,876)]:
            if kind!='app' and size==64:continue
            draw.text((x-8,top+8),str(size),fill=ink)
            frame=Image.open(io.BytesIO(pixels(kind,size))).convert('RGBA')
            center_y=top+48 if kind=='app' else top+42
            preview.paste(frame,(x-size//2,center_y-size//2),frame)
    preview.save(ROOT/'build'/'icon-preview.png')
    if args.export_directory:
        args.export_directory.mkdir(parents=True,exist_ok=True)
        mapping={'app-icon.svg':'xiaoma-brightness.svg','app-icon.png':'xiaoma-brightness.png','app.ico':'xiaoma-brightness.ico',
                 'tray-dark.svg':'xiaoma-brightness-tray-dark.svg','tray-dark.png':'xiaoma-brightness-tray-dark.png','tray-dark.ico':'xiaoma-brightness-tray-dark.ico',
                 'tray-light.svg':'xiaoma-brightness-tray-light.svg','tray-light.png':'xiaoma-brightness-tray-light.png','tray-light.ico':'xiaoma-brightness-tray-light.ico'}
        for source,name in mapping.items():shutil.copyfile(OUT/source,args.export_directory/name)
        shutil.copyfile(ROOT/'build'/'icon-preview.png',args.export_directory/'xiaoma-brightness-preview.png')
    print(json.dumps({'sources':['src/app-icon.svg','src/tray-mark.svg'],'horseRequired':True,'specRevision':REVISION,'appSizes':APP_SIZES,'traySizes':TRAY_SIZES,'palette':['#10232F','#268F91','#F6B955','#FFFFFF'],
                      'iconBytes':{name:(OUT/name).stat().st_size for name in ['app.ico','tray-dark.ico','tray-light.ico']},
                      'exportDirectory':str(args.export_directory) if args.export_directory else None},ensure_ascii=False))

if __name__=='__main__':main()
