# Game Boy static recompiler + PC runtime

Translates a 32 KB ROM-only Game Boy cartridge (SM83 machine code) into C, then
builds that C together with a small hardware runtime (PPU, APU, timers, joypad,
SDL2 window/audio/input) into a native program. Developed against *Dr. Mario*
(DMG, 32 KB, no mapper).

**Bring your own ROM.** Nothing from the game is shipped here. `tools/recomp.py`
generates code from *your* copy into `generated/`, and the program loads the ROM
at start-up for its graphics, music and data. Don't redistribute `generated/`.

## Launcher, display options, romhacks

Run the program with no arguments to get the launcher (ROM check, romhack picker, 10:9 / 4:3 / 16:9
window, scaling mode, 16 colour themes incl. GBC-style multi-colour ones, background image, volume).
Settings live in `dr_mario_launcher.ini` next to the program. A romhack (IPS/BPS/UPS, or a patched
ROM) is applied on load; if it only changes data bytes the recompiled code is used, and if it changes
any byte inside recompiled code the program silently runs everything in the interpreter instead.
`tools/make_font.py` regenerates the embedded UI font; `stb_image.h` is vendored (public domain).

## Build

Copy your ROM into the `roms/` folder (e.g. `roms/dr_mario.gb`), or point `ROM=` at it wherever it is.

Needs Python 3, a C compiler and SDL2 development files
(`apt install libsdl2-dev`, `brew install sdl2`, or the SDL2 devel package on Windows/MSYS2).

    make ROM=path/to/dr_mario.gb          # lift + compile -> build/gb
    make discover ROM=path/to/dr_mario.gb # optional: see below
    build/gb            # opens the launcher
build/gb path/to/dr_mario.gb [--hack patch.ips]   # straight into the game

or with CMake: `cmake -S . -B build -DROM=path/to/game.gb && cmake --build build`.

The binary refuses a ROM whose CRC32 differs from the one it was lifted from.

## Controls

| Game Boy | Keyboard | Pad |
|---|---|---|
| D-pad | arrows / WASD | D-pad |
| A / B | X / Z (or K / J) | east / south buttons |
| Start / Select | Enter / Right Shift or Backspace | Start / Back |
| | Esc quit, F11 fullscreen | |

Options: `--scale N`, `--fullscreen`, `--no-audio`, `--turbo`.

## How it works

* `tools/sm83.py` – the opcode table; semantics for every instruction, written once.
* `tools/recomp.py` – recursive-descent discovery from the reset/RST/interrupt
  vectors and entry point, including `rst`-style jump tables (`pop hl ... jp hl`
  handlers are detected automatically). Emits one C label per block leader, cycle
  counts per instruction, an interrupt check after every instruction, and a
  `switch` dispatcher for returns and computed jumps. Also emits `interp.c`, an
  interpreter generated from the same table.
* Anything that is not in the recompiled set (code copied to RAM/HRAM such as the
  OAM-DMA stub, or ROM code the static pass missed) transparently runs in the
  interpreter, so the program is always correct, just slightly slower there.
* `tools/discover.py` (`make discover`) plays the game headless with random
  button presses, collects ROM addresses that fell back to the interpreter, and
  feeds them back as extra roots until nothing new shows up.

## Verification done

* Blargg `cpu_instrs` (all 11 individual ROMs) pass in both the interpreter and the
  lifted code, with zero interpreter fallbacks.
* For Dr. Mario, lifted code and interpreter produce identical per-frame video
  hashes, cycle counts and registers over 5000 fuzzed-input frames on 4 seeds.
  `build/gb --headless --interp --fuzz N --frames F --hash rom.gb` vs. the same
  without `--interp` is the check to re-run after any change.
* After `make discover`, no ROM address runs outside the recompiled code.

## Limits

* **ROM-only, 32 KB.** Cartridges with a mapper (MBC1/3/5, bigger ROMs, cart RAM / saves)
  need bank-aware addressing in the lifter; it refuses them.
* DMG only (no CGB palettes, double speed, VRAM banks).
* PPU is scanline-based (registers sampled at end of each line's drawing period);
  no mid-line effects, no VRAM/OAM access blocking. STOP is treated as a 2-byte NOP;
  the HALT bug isn't modelled.
* APU is a straightforward channel model with nearest-sample output and a light
  low-pass filter, not cycle-exact.
* Audio output and window code were exercised against SDL's dummy drivers here,
  not on a real display or sound card.
