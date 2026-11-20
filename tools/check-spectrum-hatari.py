#!/usr/bin/env python3
"""End-to-end ST/STE raster test. Requires Hatari, EmuTOS, Pillow and built CLI.
Run: python3 tools/check-spectrum-hatari.py [--image examples/maui.jpg]
Compares every emulated pixel with an independent SPU decode, exercises all
four ST wakeup states, rebuilds generated C++, and checks the return to TOS.
"""
import argparse, os, pathlib, shutil, socket, subprocess, tempfile, time
from PIL import Image
root=pathlib.Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser()
p.add_argument('--image',default=str(root/'examples/maui.jpg'))
p.add_argument('--hatari',default=shutil.which('hatari'))
p.add_argument('--tos',default='/opt/homebrew/share/hatari/tos.img')
p.add_argument('--output',default=str(root/'build/atari-spectrum/verification'))
a=p.parse_args();out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
def command(args, **kw):
    return subprocess.run([str(x) for x in args],check=True,**kw)
def decode(data,ste):
    pixels=[]
    for y in range(199):
        for x in range(320):
            reg=0
            for plane in range(4):
                offset=(y+1)*160+x//16*8+plane*2
                word=int.from_bytes(data[offset:offset+2],'big')
                reg|=((word>>(15-x%16))&1)<<plane
            cut=10*reg+(-5 if reg%2 else 1)
            bank=0 if x<cut else 1 if x<cut+160 else 2
            offset=32000+(y*48+bank*16+reg)*2
            w=int.from_bytes(data[offset:offset+2],'big')
            # Hatari's ST DAC uses even STE intensities (0..14), not 0..15.
            pixels.append(tuple((((w>>s)&7)*2+(((w>>(s+3))&1) if ste else 0))*17 for s in (8,4,0)))
    return pixels
def launch(exe,machine,timing='ws3',interactive=False):
    stem=f'{exe.stem}-{machine}-{timing}';png=out/(stem+'.png')
    shutil.copyfile(exe,out/'PICTURE.PRG')
    capture=out/'capture.ini';capture.write_text(f'screenshot {png}\nquit\n')
    start=out/'start.ini';start.write_text(f'b VBL = 600 :trace :once :file {capture}\n')
    args=[a.hatari,'--tos',a.tos,'--machine',machine,'--cpulevel','0','--cpuclock','8','--video-timing',timing,
          '--memsize','1','--monitor','rgb','--tos-res','low','--borders','false','--statusbar','false',
          '--drive-led','false','--zoom','1','--sound','off','--fast-forward','true','--confirm-quit','false',
          '--harddrive',str(out),'--auto','C:\\PICTURE.PRG']
    env={**os.environ,'SDL_VIDEODRIVER':'dummy'}
    with open(out/(stem+'.log'),'w') as log:
        if not interactive:
            command(args+['--parse',start],env=env,stdout=log,stderr=log,timeout=30)
        else:
            with tempfile.TemporaryDirectory() as d:
                with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
                    server.bind(d+'/control');server.listen(1);server.settimeout(10)
                    proc=subprocess.Popen(args+['--control-socket',d+'/control'],env=env,stdout=log,stderr=log)
                    try:
                        c,_=server.accept()
                        with c:
                            time.sleep(1)
                            c.sendall(b'hatari-event keypress 0x1\n');time.sleep(.4)
                            c.sendall(f'hatari-debug screenshot {png}\n'.encode());time.sleep(.1)
                            c.sendall(b'hatari-debug quit\n');proc.wait(timeout=10)
                    finally:
                        if proc.poll() is None:proc.kill();proc.wait()
    im=Image.open(png).convert('RGB')
    assert im.size==(640,400),im.size
    return im
for mode,ste in [('stf-spectrum512',False),('ste-spectrum4096',True)]:
    base=out/mode;exe=base.with_suffix('.exe');cpp=base.with_suffix('.cpp')
    for file in (exe,cpp):command([root/'build/png2amiga','--mode',mode,'--dither','opt-checker',a.image,file],stdout=subprocess.DEVNULL)
    rebuilt=out/(mode+'-rebuilt.exe');command([root/'build-atari.sh',cpp,rebuilt])
    assert exe.read_bytes()==rebuilt.read_bytes(),'source/direct executable mismatch'
    reference=decode(exe.read_bytes()[-51104:],ste)
    for machine,timing in ([('ste','ws3')] if ste else [('st',f'ws{i}') for i in range(1,5)]+[('ste','ws3')]):
        im=launch(exe,machine,timing)
        actual=[im.getpixel((x*2,(y+1)*2)) for y in range(199) for x in range(320)]
        bad=sum(x!=y for x,y in zip(actual,reference));assert bad==0,(mode,machine,timing,bad)
        assert all(im.getpixel((x,0))==(0,0,0) for x in range(640)),'first scanline'
        print(f'{mode} on {machine}/{timing}: all 63680 pixels match')
    desktop=launch(exe,'ste' if ste else 'st',interactive=True)
    # GEM menu bar is white and black, whereas the viewer always has a black first line.
    assert min(desktop.getpixel((0,0)))>=238,'Escape did not restore desktop'
    print(f'{mode}: Escape restored TOS desktop')
    if ste:
        rejected=launch(exe,'st')
        assert min(rejected.getpixel((0,0)))>=238,'STE executable did not reject ST'
        print('Spectrum 4096 rejected original ST')
