"""Render the 320x480 player layout using the production disc raster and boot logo."""
import argparse
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import numpy as np

def render(disc, output, bank):
    base=Image.new("RGB",(320,480),"#09090e")
    logo=Image.open(bank/"boot-preview.png").convert("RGB").resize((120,180))
    base.paste(logo.crop((0,68,120,110)),(12,10))
    words=np.fromfile(disc,dtype="<u2").reshape(208,208).astype(np.uint32)
    base.paste(Image.fromarray(np.stack(((words>>11)*255//31,((words>>5)&63)*255//63,(words&31)*255//31),-1).astype("uint8")),(56,64))
    try: font=ImageFont.truetype("DejaVuSans.ttf",16)
    except OSError: font=ImageFont.load_default(size=16)
    frames=[]
    for i in range(54):
        f=base.copy()
        d=ImageDraw.Draw(f)
        for text,xy,col in [("NOW PLAYING",(185,26),"#bb92fb"),("Evening test",(20,292),"#f4f2fc"),("Project Aura",(20,322),"#b4acbf"),(f"00:0{i//10}  /  00:08",(20,378),"#d0cbd8"),("Playing    |    VOL 45%",(20,411),"#c084fc"),("Press BOOT to stop",(20,452),"#aba5b7")]:
            d.text(xy,text,font=font,fill=col)
        d.rectangle((20,366,299,370),fill="#34303f")
        d.rectangle((20,366,20+int(280*i/80),370),fill="#a855f7")
        for n,(r,size) in enumerate(((78,13),(48,6))):
            a=i*.12+n*2.45;x=160+int(math.cos(a)*r);y=168+int(math.sin(a)*r)
            d.ellipse((x-size//2,y-size//2,x+size//2,y+size//2),fill="#ffffff" if not n else "#debaff")
        frames.append(f)
    output.mkdir(exist_ok=True,parents=True)
    frames[20].save(output/"music-player-preview.png")
    frames[0].save(output/"music-player.gif",save_all=True,append_images=frames[1:],loop=0,duration=100)
if __name__=="__main__":
    p=argparse.ArgumentParser()
    p.add_argument("disc",type=Path);p.add_argument("out",type=Path)
    args=p.parse_args()
    render(args.disc,args.out,Path(__file__).resolve().parents[1]/"main/boards/anime-companion-s3/web")
