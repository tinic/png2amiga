#!/usr/bin/env python3
from pathlib import Path
import json
import numpy as np
from PIL import Image
root=Path('build/to8-pairs/gallery')
intens=np.array([0,96,124,143,159,172,183,194,203,212,220,228,235,242,248,255],dtype=np.uint8)
count=0
for folder in root.iterdir():
 if not folder.is_dir() or not (folder/'results.json').exists():continue
 report=json.loads((folder/'results.json').read_text())
 assert all(report[arm]['gain']>=0 for arm in ['single','paired'])
 for arm in ['before','single','paired']:
  codes=json.loads((folder/f'{arm}.json').read_text())['palette_codes']
  assert len(codes)==16 and all(0<=c<=4095 for c in codes)
  pal=np.array([[intens[c>>8],intens[(c>>4)&15],intens[c&15]] for c in codes])
  raw=np.frombuffer((folder/f'{arm}.bin').read_bytes(),dtype=np.uint8)
  assert len(raw)==16000
  attr=raw[:8000].astype(np.int32);bits=raw[8000:]
  bg=(attr&7)|(((~attr)>>4)&8);fg=((attr>>3)&7)|(((~attr)>>3)&8)
  decoded=np.where((bits[:,None] & (0x80>>np.arange(8)))!=0,fg[:,None],bg[:,None]).astype(np.uint8)
  idx=np.frombuffer((folder/f'{arm}.idx').read_bytes(),dtype=np.uint8)
  assert np.array_equal(decoded.flatten(),idx)
  rgb=np.array(Image.open(folder/f'{arm}.png').convert('RGB'))
  assert np.array_equal(pal[decoded].reshape((200,320,3)),rgb)
  count+=1
print(count,'TO8 VRAM/palette replays passed; all previews obey two colors per 8x1 cell')
