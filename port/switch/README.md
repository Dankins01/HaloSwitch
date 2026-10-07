# Nintendo Switch (experimental)

An `.nro` for the Homebrew Menu on a Switch running Atmosphère. It runs the
same game image as the Android port: the decompilation, the Linux platform
layer and a small musl runtime, compiled as 32-bit-pointer AArch64 code.
What is new here is the host around it, written for libnx instead of
Android.

**Status: untested on hardware.** The host compiles against the real
libnx, newlib, SDL2 and mesa headers, and every function the game imports
is provided, but nobody has run it on a console yet. Expect the first runs
to stop early; `host.txt` (below) says where.

## What you need

- A Switch that runs Atmosphère (custom firmware), with the Homebrew Menu.
- Your own copy of Halo: Combat Evolved for the original Xbox, as a disc
  image (`.iso` or `.xiso`), or the `maps` folder copied out of one. This
  project does not include or download the game.
- About 2 GB free on the SD card.

## Build

The build has two halves with different toolchains, so it runs in GitHub
Actions (`.github/workflows/switch.yml`):

1. Fork the repository on GitHub and push this branch to the fork.
2. In the fork's **Actions** tab, open **switch** and select **Run workflow**
   (pushes to branches named `switch*` run it too).
3. When it finishes, download the **opence-switch** artifact. It holds
   `opence.nro`.

To build locally instead, you need what both jobs install:

```sh
# the game image (Linux, with clang 18+ and the Android NDK)
python3 configure.py --release
ninja build/android/halo_guest.elf build/android/host/host_import_table.c

# the host (devkitPro with switch-dev, switch-sdl2, switch-mesa, switch-libdrm_nouveau)
make -C port/switch GUEST=../../build/android
```

## Install and play

1. Copy `opence.nro` to `/switch/opence/opence.nro` on the SD card.
2. Copy your disc image to `/switch/opence/` (any name ending in `.iso` or
   `.xiso`). On a FAT32 card a file cannot exceed 4 GB: use an xiso (made
   with extract-xiso), or copy the `maps` folder to `/switch/opence/maps`.
3. Start the Homebrew Menu **from a game**: hold **R** while starting any
   game, not from the Album. The Album gives homebrew a few hundred MB of
   memory; the game needs more.
4. Start **OpenCE**. The first start copies `maps` out of the disc image
   (1.8 GB, a few minutes); you can delete the image afterwards.

If it says the fixed guest memory is not free, start it again: the kernel
lays out each process's memory at random, and some layouts put one of its
regions where the game's memory must be.

| Path on the SD card | What |
| --- | --- |
| `/switch/opence/maps/` | the game data |
| `/switch/opence/save/` | saved games and profiles |
| `/switch/opence/config.toml` | settings ([port/linux/README.md](../linux/README.md#settings)) |
| `/switch/opence/debug.txt` | the game's log |
| `/switch/opence/host.txt` | the Switch host's log |

Leaving the game returns to the Home menu.

The Homebrew Menu shows the build as the version (`0.1.0-<build>`), the
same id as the first line of `host.txt`.

### Icon

The icon is `port/switch/art/icon.jpg`, a 256x256 JPEG. Replace it with an
image of your own and build again to change it.

### On the Home screen

The Homebrew Menu is the only launcher this project makes. To start the
game from the Switch's own Home screen, you can make an NSP "forwarder"
from `opence.nro` yourself, with a tool such as
[NTON](https://pypi.org/project/nton) (`nton build opence.nro`). It needs
the `prod.keys` of your own console. A forwarder starts as a full
application, so the game gets all its memory without holding R. Read the
tool's warnings first: installing forwarders needs signature patches and
can get a console banned from online services. The forwarder opens the
`.nro` at the path it was made for: keep `/switch/opence/opence.nro`
there. Forwarders are not tested with this port.

### Faster clocks (optional)

Create an empty file `/switch/opence/boost.txt` to run the CPU at 1785 MHz
(the rate of Nintendo's own CPU boost mode) and the GPU at the top of
Nintendo's normal range (460.8 MHz handheld, 768 MHz docked). It uses more
battery and runs warmer. The original clocks come back when the game ends;
after a crash, the next program you start resets them. `host.txt` logs the
clocks at start-up and on docking.

Every 10 seconds `host.txt` logs the frame rate, the time of a frame, how
much of it the swap took (the GPU's work and the display; the rest is the
game's work on the CPU) and the texture write faults.

With an empty `/switch/opence/profile.txt`, a sampling profiler runs: 500
times a second it notes where each game thread is, and every 30 seconds
`host.txt` gives, for each thread, how busy it was and the places it was
sampled most (addresses in the game image, to name with
`llvm-symbolizer --obj=halo_guest.elf`, and offsets into `opence.elf`):
where it worked, which calls it waited in, and which game functions the
time in `opence.elf` (the GL driver, mostly) was for. It costs a little
speed; delete the file to stop it.

The game's main thread has core 0 to itself; its other threads, audio
among them, share cores 1 and 2.

For a steady frame rate, set `interpolation = false` under `[display]` in
`config.toml`. The default draws a blended frame for every 60 Hz refresh,
and when the Switch cannot keep that up the frame rate jumps between 30,
20 and 15. Off, the game draws its original 30 frames a second.

## Controls

Pro Controller, or both Joy-Con (attached to the console or in a grip);
up to four players. Buttons are read by position, as on an Xbox
controller: the bottom face button (**B**) is Xbox A (jump).

| Switch | Xbox | In the game |
| --- | --- | --- |
| left stick, right stick | left stick, right stick | move, look |
| ZR | right trigger | fire |
| ZL | left trigger | throw a grenade |
| B (bottom) | A | jump, accept |
| A (right) | B | melee, back |
| Y (left) | X | action, reload |
| X (top) | Y | change the weapon |
| L | white | flashlight |
| R | black | change the grenade |
| stick clicks | stick clicks | crouch, zoom |
| + | start | pause menu |
| − | back | |

Rumble uses the controller's HD rumble.

Joy-Con and Pro Controller ZL and ZR are on/off switches, so the triggers
are either fully pressed or not. A GameCube controller (through an
adapter the Switch supports) has analog triggers, and the game reads them
as such. It uses its own layout: A jumps, B melees, X and Y as marked, Z is
the right shoulder.

Not yet: a single Joy-Con as a controller, touch, and UPnP for internet
games (system link on a local network uses the console's Wi-Fi).

## When it stops

- An error dialog or a return to the Home menu: read `host.txt`, then
  `debug.txt`.
- A crash (Atmosphère's crash screen): `host.txt` ends with the registers.
  When the program counter is in the game image, it gives an
  `llvm-symbolizer --obj=halo_guest.elf 0x...` line; `halo_guest.elf` is in
  the workflow's `switch-guest` artifact. Atmosphère also writes a report to
  `/atmosphere/crash_reports/`.
- Textures that never update, or a crash on the first write to a texture:
  the host relies on the kernel passing memory faults to the program, which
  Atmosphère does unless `disable_user_exception_handlers` is set in
  `exosphere.ini`.

## How it works

Read [port/android/README.md](../android/README.md) ("How the port
operates") first: the game image and its contract with the host
(`port/android/include/halo_android_abi.h`, `host_imports.list`) are that
port's, unchanged. This host (`port/switch/host`) replaces the Android one:

| File | Does |
| --- | --- |
| `switch_main.c` | start-up, the SD card layout, the first start's copy of the game data (`port/linux/src/xiso.c`), logging |
| `switch_memory.c` | the guest's memory below 4 GB, and write tracking for the texture cache |
| `switch_exception.s`, `switch_exception.c` | memory faults: a watched page's first write, or a crash report |
| `switch_loader.c` | loads `halo_guest.elf` from the NRO's romfs |
| `switch_thread.c` | threads whose stacks are in guest memory |
| `switch_syscall.c` | the guest's Linux system calls, on newlib and libnx |
| `switch_sdl.c` | the guest's SDL3 calls, on SDL2 and libnx's controller API |
| `switch_gl.c` | OpenGL ES, from mesa |
| `switch_files.c`, `switch_net.c` | `port/linux/src/posix.h` for the Switch |

The parts that differ most from Android:

**Memory below 4 GB.** Horizon has no `mmap`. Guest memory is heap memory
aliased at a fixed address with `svcMapProcessCodeMemory`, as hbloader
loads an NRO. An alias can be made read-execute or read-write once, with
`svcSetProcessMemoryPermission`. After that, read-write aliases can only be
reprotected (none, read, read-write) with `svcSetMemoryPermission`. So the
image is assembled in heap memory before it is mapped, and all other guest
memory is read-write data that is protected afterwards. There is no
reserving address space without memory behind it, so the 128 MB Xbox
window is committed at start-up and the rest comes in 64 MB pools.

**Faults instead of signals.** The texture cache write-protects pages and
needs to hear of the first write to each. Horizon delivers the fault to
the process's entry point, and hbloader forwards it to the NRO. libnx's
handler cannot resume the faulting instruction, so `switch_exception.s`
replaces it. It saves the registers the kernel does not, makes the page
writeable, and returns with `svcReturnFromException(0)`, which reruns the
write.

**Stacks.** libnx always maps a thread's stack above 4 GB, so threads that
run game code switch onto a stack allocated in guest memory.

**SDL3 on SDL2.** devkitPro has SDL2 only. The guest's SDL3 values are in
`sdl3_values.h`, generated from SDL 3.4.16's headers by
`tools/sdl3_values_probe.c`. Several differ from SDL2's (the GL context
attributes, all event types), and they are translated. Controllers come
from libnx directly, so button positions do not depend on SDL2's mapping.

**System calls.** Linux's open flags, `struct stat`, directory entries,
futexes (`svcWaitForAddress`/`svcSignalToAddress`) and error numbers (which
differ from newlib's above 34) are translated. Directories opened by the
guest get descriptors of their own, from `0x10000`.
