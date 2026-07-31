#!/usr/bin/env python3
"""Paired evaluation for deterministic sprite upscalers.

Usage: python3 evaluate_upscalers.py ./celup3 ./celup4 ./celup5
       python3 evaluate_upscalers.py ./celup_lab:cubic ./celup_lab:mitchell ./celup_lab:lanczos3

A candidate may be EXE or EXE:MODE.  EXE:MODE runs the same binary with
`--mode MODE`, which is useful for comparing celup_lab modes.

The high-resolution render is ground truth. A 4x4 box reduction in
premultiplied linear RGBA makes the low-resolution input. Every candidate then
returns to the original resolution. Metrics are reported in linear premultiplied
RGBA, which avoids hidden-RGB/transparent-pixel errors.
"""
import subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

S,N=4,96

def candidate(spec):
 if spec.lower().startswith('pil:'):
  return ('pil', spec.split(':', 1)[1].lower())
 p=Path(spec); mode=None
 if ':' in spec:
  left,right=spec.rsplit(':',1)
  if left and right:
   p=Path(left); mode=right
 return (p.resolve(),mode)
def cname(c):
 exe,mode=c
 if exe == 'pil':
  return 'pil:' + mode
 return exe.name+((':'+mode) if mode else '')
def run_candidate(c, inp, out):
 exe,mode=c
 if exe == 'pil':
  im = Image.open(inp)
  w, h = im.size
  res = {'nearest': Image.Resampling.NEAREST, 'bilinear': Image.Resampling.BILINEAR,
         'bicubic': Image.Resampling.BICUBIC, 'lanczos': Image.Resampling.LANCZOS}.get(mode, Image.Resampling.BICUBIC)
  im.resize((w*4, h*4), res).save(out)
  return
 cmd=[exe,inp,out,'4']
 if mode: cmd += ['--mode',mode]
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)

def lin(x):
 x=x/255.; return np.where(x<=.04045,x/12.92,((x+.055)/1.055)**2.4)
def scene(name):
 z=N*S; im=Image.new('RGBA',(z,z),(0,0,0,0));d=ImageDraw.Draw(im)
 if name=='diag':
  d.polygon([(0,z),(0,3*z//4),(z,z//4),(z,z//2)],fill=(20,210,255,255));d.line((0,3*z//4,z,z//4),fill='white',width=2*S)
 elif name=='curves':
  d.ellipse((12*S,14*S,84*S,86*S),fill=(248,170,20,255));d.ellipse((30*S,30*S,67*S,67*S),fill=(30,20,70,255))
 elif name=='gradient':
  a=np.zeros((z,z,4),np.uint8);x=np.linspace(0,255,z,dtype=np.uint8);a[:,:,0]=x;a[:,:,1]=255-x;a[:,:,2]=80;a[:,:,3]=255;return a
 elif name=='axis':
  d.rectangle((0,0,z//2,z),fill=(30,40,70,255));d.rectangle((z//2,0,z,z),fill=(250,190,30,255)); d.line((0,z//2,z,z//2),fill=(255,255,255,255),width=2*S)
 elif name=='shallow':
  d.rectangle((0,0,z,z),fill=(20,25,48,255));d.line((-z//8,3*z//4,9*z//8,z//2),fill=(255,210,70,255),width=3*S)
 elif name=='thin':
  d.rectangle((0,0,z,z),fill=(235,230,215,255));d.line((z//12,11*z//12,11*z//12,z//12),fill=(35,20,80,255),width=S)
 elif name=='corner':
  d.polygon([(z//8,z//8),(7*z//8,z//8),(7*z//8,7*z//8),(z//2,5*z//8),(z//8,7*z//8)],fill=(180,60,180,255));d.line([(z//8,z//8),(7*z//8,z//8),(7*z//8,7*z//8),(z//2,5*z//8),(z//8,7*z//8),(z//8,z//8)],fill=(255,255,255,255),width=S)
 elif name=='parallel':
  d.rectangle((0,0,z,z),fill=(15,22,36,255));d.line((-z//10,3*z//4,11*z//10,z//4),fill=(240,190,55,255),width=2*S);d.line((-z//10,7*z//8,11*z//10,3*z//8),fill=(240,190,55,255),width=2*S)
 elif name=='alpha':
  d.ellipse((12*S,12*S,84*S,84*S),fill=(25,180,255,160));d.line((10*S,80*S,85*S,10*S),fill=(255,60,20,255),width=3*S)
 return np.asarray(im)
def pm(a):
 o=np.empty_like(a,dtype=np.float32);o[:,:,:3]=lin(a[:,:,:3])* (a[:,:,3:]/255.);o[:,:,3]=a[:,:,3]/255.;return o
def down(a): return a.reshape(N,S,N,S,4).mean((1,3))
def encode_pm(p):
 a=np.clip(p[:,:,3:],0,1)
 rgb=np.divide(p[:,:,:3],np.maximum(a,1e-8))
 sr=np.where(rgb<=.0031308,12.92*rgb,1.055*rgb**(1/2.4)-.055)
 rgba=np.dstack((np.clip(sr*255,0,255),a*255)).astype(np.uint8)
 return Image.fromarray(rgba,'RGBA')
def main():
 cands=[candidate(x) for x in sys.argv[1:]] or [(Path('./celup5').resolve(),None)]
 with tempfile.TemporaryDirectory() as td:
  td=Path(td)
  print('metric: linear premultiplied RGBA MAE; lower is better')
  for name in ('diag','curves','gradient','axis','shallow','thin','corner','parallel','alpha'): 
   truth=pm(scene(name)); low=down(truth);inp=td/(name+'.webp');encode_pm(low).save(inp,lossless=True)
   row=[]
   for cand in cands:
    out=td/(cname(cand).replace('/','_')+'-'+name+'.webp');run_candidate(cand,inp,out)
    got=pm(np.asarray(Image.open(out).convert('RGBA'))); # ignore border extension
    err=np.abs(got[4:-4,4:-4]-truth[4:-4,4:-4]);row.append(f'{err.mean():.5f}')
   print(f'{name:9s}', '  '.join(f'{cname(c)}: {v}' for c,v in zip(cands,row)))
if __name__=='__main__':main()
