#!/usr/bin/env python3
"""Regression/quality test for celup3.
Creates analytic high-resolution references, downsamples them with Lanczos,
then compares 4x celup3 output to the references. This is a controlled test of
edges, diagonals, curves, gradients, and transparent colour boundaries.

Requires: Python Pillow, numpy; ./celup3 built. Run: python3 test_celup3.py
"""
import subprocess, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

S, N = 4, 96

def scene(kind):
    z=N*S; im=Image.new('RGBA',(z,z),(0,0,0,0)); d=ImageDraw.Draw(im)
    if kind=='diagonals':
        d.polygon([(0,z),(0,z*3//4),(z,z//4),(z,z//2)], fill=(20,210,255,255)); d.line((0,z*3//4,z,z//4),fill=(255,255,255,255),width=3*S)
    elif kind=='circles':
        d.ellipse((18*S,18*S,78*S,78*S),fill=(255,179,0,255)); d.ellipse((31*S,31*S,65*S,65*S),fill=(38,34,50,255))
    elif kind=='gradient':
        a=np.zeros((z,z,4),np.uint8); x=np.linspace(0,1,z)[None,:]; a[:,:,0]=(255*x);a[:,:,1]=(255*(1-x));a[:,:,2]=90;a[:,:,3]=255; im=Image.fromarray(a,'RGBA')
    elif kind=='alpha':
        d.rectangle((8*S,8*S,88*S,88*S),fill=(255,20,50,0)); d.ellipse((16*S,16*S,80*S,80*S),fill=(30,180,255,190))
    return im

def main():
    import sys
    exe=Path(sys.argv[1] if len(sys.argv) > 1 else './celup3').resolve()
    if not exe.exists(): raise SystemExit(f'Build {exe} first')
    vals=[]
    with tempfile.TemporaryDirectory() as t:
      t=Path(t)
      for name in ('diagonals','circles','gradient','alpha'):
        ref=scene(name); low=ref.resize((N,N),Image.Resampling.LANCZOS)
        ip,op=t/(name+'.webp'),t/(name+'-up.webp'); low.save(ip,lossless=True)
        subprocess.run([exe,ip,op,'4'],check=True,stdout=subprocess.DEVNULL)
        got=np.asarray(Image.open(op).convert('RGBA'),dtype=np.float32); truth=np.asarray(ref,dtype=np.float32)
        # Ignore a 4px outer boundary: edge extension has no analytic neighbours.
        got=got[4:-4,4:-4];truth=truth[4:-4,4:-4]
        mae=np.abs(got-truth).mean(); p99=np.percentile(np.abs(got-truth),99)
        vals.append(mae); print(f'{name:10s} MAE={mae:6.2f}  p99={p99:6.1f}')
    print(f'mean MAE={np.mean(vals):.2f} (lower is better; retain as a regression baseline)')
if __name__=='__main__': main()
