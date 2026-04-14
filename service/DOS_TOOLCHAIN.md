# DOS toolchain setup for the compile service

The compile service turns a png2amiga-generated DOS viewer `.cpp` into:

- `dos-exe` — a runnable DJGPP `.exe` (CWSDPMI-embedded, works on any DOS)
- `img`     — a 1.44 MB FAT12 FreeDOS-bootable floppy image with `AUTOEXEC.BAT`
              that auto-runs the viewer on boot

## What's committed (shared, small, GPL-or-public-domain)

All under `service/toolchain/dos/`:

| File | Size | Origin | License |
|---|---|---|---|
| `KERNEL.SYS` | 48 KB | FreeDOS kernel 2044, 386 build (renamed from `KERNL386.SYS`) | GPL |
| `COMMAND.COM` | 86 KB | FreeCOM 0.86 | GPL |
| `boot12.bin` | 512 B | FAT12 boot sector built with `nasm -DISFAT12 boot.asm` from FreeDOS kernel source | GPL |
| `CWSDSTUB.EXE` | 22 KB | CWSDPMI 7 embedded stub, from `csdpmi7b.zip` | public domain |

Total committed: ~160 KB.

## What the deployer still has to drop in

Under `service/toolchain/dos/`:

### DJGPP cross-compiler (~40 MB)

```bash
cd /var/www/png2amiga/service/toolchain/dos
curl -LO https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2
tar -xf djgpp-linux64-gcc1220.tar.bz2
rm djgpp-linux64-gcc1220.tar.bz2
```

Expected final layout:

```
service/toolchain/dos/
├── KERNEL.SYS        (committed)
├── COMMAND.COM       (committed)
├── boot12.bin        (committed)
├── CWSDSTUB.EXE      (committed)
└── djgpp/            (deploy drop-in, ~40 MB)
    ├── bin/i586-pc-msdosdjgpp-g++
    ├── i586-pc-msdosdjgpp/bin/exe2coff
    └── ...
```

### Host packages

```bash
sudo apt install bubblewrap mtools
```

(`bubblewrap` was already required for the Amiga compile path.)

## Bootable floppy template

The 1.44 MB `freedos-bootable-1.44M.img` template is built automatically on
the first `img` request and cached in `service/toolchain/dos/` thereafter.
It's just:

1. 1 474 560-byte zero-filled file
2. `mformat` with our `boot12.bin` as the boot sector
3. `mcopy KERNEL.SYS COMMAND.COM` onto it
4. Empty `AUTOEXEC.BAT` (gets overwritten per request with `VIEWER.EXE\n`)

If you ever want to force a rebuild (e.g., after updating `KERNEL.SYS`),
delete `service/toolchain/dos/freedos-bootable-1.44M.img` — it regenerates
on the next request.

## Per-request floppy build

For `/api/compile?format=img`:

1. Compile the user's `.cpp` with DJGPP, strip the default stub with
   `exe2coff`, prepend `CWSDSTUB.EXE` → self-contained viewer `.exe`.
2. `shutil.copy` the cached template to a fresh temp file.
3. `mcopy viewer.exe ::VIEWER.EXE`
4. `mcopy "AUTOEXEC.BAT" ::AUTOEXEC.BAT` (content: `@ECHO OFF\r\nVIEWER.EXE\r\n`)
5. Return the 1.44 MB image.

Both mcopy calls run inside a `bwrap` sandbox that only sees the request's
tempdir (and /usr for the mcopy binary).

## Verifying the deploy

```bash
systemctl restart png2amiga-compile.service
journalctl -u png2amiga-compile.service -n 10
```

Expected log on startup:

```
DOS toolchain OK at /var/www/png2amiga/service/toolchain/dos (DJGPP + FreeDOS bits + CWSDPMI stub)
Compile server listening on http://127.0.0.1:3001 (sandboxed)
```

If DJGPP or mtools are missing:

```
DOS toolchain NOT ready: <reason>  — dos-exe/img requests will 503 until this is fixed
```

The Amiga path keeps working regardless of DOS-side setup.

## Smoke test

```bash
# Make a DOS viewer source locally
./build/png2amiga --mode vga-13h --symbol VGA13H \
  examples/fantasy1.png /tmp/vga13h.cpp

# Compile remotely → .exe
curl -X POST -H 'Content-Type: text/plain' \
     --data-binary @/tmp/vga13h.cpp \
     'http://127.0.0.1:3001/api/compile?format=dos-exe' \
     -o /tmp/vga13h.exe

# Or → bootable floppy
curl -X POST -H 'Content-Type: text/plain' \
     --data-binary @/tmp/vga13h.cpp \
     'http://127.0.0.1:3001/api/compile?format=img' \
     -o /tmp/vga13h.img

# Boot the floppy
qemu-system-i386 -fda /tmp/vga13h.img -boot a
```

## License note

Files under `service/toolchain/dos/` are derived from:

- **FreeDOS** (`KERNEL.SYS`, `COMMAND.COM`, `boot12.bin`) — GPL-2.0+. Source is in
  the matching github.com/FDOS/kernel and github.com/FDOS/freecom releases.
- **CWSDPMI** (`CWSDSTUB.EXE`) — public-domain DOS/DPMI server by Charles W.
  Sandmann. Source at https://sandmann.dotster.com/cwsdpmi/.

Redistributing as part of a service is fine under these licenses.
