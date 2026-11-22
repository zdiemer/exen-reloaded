#!/usr/bin/env python3
"""Probe every game at several handset screen sizes and score each run by its uniform border
(empty margins mean the screen is too big; none means the game fills it). Used to build
data/gamedb.tsv. Usage: python tools/harness/probe_sizes.py OUTDIR GAMES_DIR"""
import subprocess, sys, os, glob, json, re
from concurrent.futures import ThreadPoolExecutor
from PIL import Image
ROOT=__import__('pathlib').Path(__file__).resolve().parents[2]
EXE=str(ROOT/'build'/('exen-run.exe' if os.name=='nt' else 'exen-run'))
SIM=str(ROOT/'vendor/gensimu/gensimu.exe')
OUT=sys.argv[1]; GAMES=sorted(glob.glob(sys.argv[2]+'/*.exn'))
SIZES=['101x80','120x143','128x141','132x176','112x112','128x160','128x128']
KEYS=','.join(f'{t}:{k}' for t,k in [(60,'ok'),(100,'ok'),(140,'ok'),(180,'fire'),(220,'ok'),(260,'fire'),(300,'right/5'),(340,'left/5'),(380,'up/3'),(420,'fire')])
def border(img):
    w,h=img.size; px=img.load()
    def uniform(coords):
        vals={px[x,y] for x,y in coords}; return len(vals)==1
    m={}
    for side in 'ltrb':
        n=0
        while n < (w if side in 'lr' else h)//2:
            if side=='l': c=[(n,y) for y in range(h)]
            elif side=='r': c=[(w-1-n,y) for y in range(h)]
            elif side=='t': c=[(x,n) for x in range(w)]
            else: c=[(x,h-1-n) for x in range(w)]
            if not uniform(c): break
            n+=1
        m[side]=n
    return m
def run(game,size):
    name=os.path.splitext(os.path.basename(game))[0]; d=f'{OUT}/{name}/{size}'; os.makedirs(d,exist_ok=True)
    r=subprocess.run([EXE,'--sim',SIM,'--quiet','--frames','600','--size',size,'--dump',d,'--dump-every','50','--keys',KEYS,game],capture_output=True,text=True,timeout=120)
    st=re.search(r'status: (.*)',r.stdout); st=st.group(1) if st else 'crash rc=%d'%r.returncode
    fr=sorted(glob.glob(d+'/*.png'))[2:]
    bs=[border(Image.open(f).convert('RGB')) for f in fr]
    avg={k:round(sum(b[k] for b in bs)/len(bs),1) for k in 'ltrb'} if bs else None
    return name,size,st,avg,len(fr)
res=[]
with ThreadPoolExecutor(6) as ex:
    for r in ex.map(lambda a: run(*a), [(g,s) for g in GAMES for s in SIZES]): res.append(r)
json.dump(res,open(OUT+'/probe.json','w'),indent=1)
for n,s,st,avg,k in res: print(f'{n[:20]:20} {s:8} {st[:40]:40} {avg} frames={k}')
