# NDS Music Player (3DS)

A Nintendo 3DS homebrew app that plays Nintendo DS music **straight from `.nds` ROM files** and from **NCSF / miniNCSF** rips.
It finds the game's sound archive (SDAT) inside the ROM, lists every sequence and stream, and plays them with a software
recreation of the DS sound hardware. Nothing gets extracted or converted first.

## Features

- Browse the SD card for `.nds` / `.srl` ROMs and `.ncsf` / `.minincsf` files (with `_lib` `.ncsflib` support)
- Plays games that use Procyon's DSE sound engine (tested with Pokémon Mystery Dungeon: Explorers of Sky;
  Explorers of Time/Darkness use the same engine), from ROMs and `.mini2sf` sets, without emulation (see below)
- Lists all sequences (SSEQ) and streams (STRM) in a ROM, using their internal names when the game kept them
- Sound effects from sequence archives (SSAR) are listed after the music (green, `E<archive>.<entry>`); pressing A
  plays one on top of the current music (up to 4 at once), and X stops everything
- Shows the game's DS Menu icon (including animated DSi icons) and full title, in your 3DS's language when the game has it
- Sequence player: full SSEQ command set (loops, calls, random/variable/if prefixes, tie, portamento, sweep,
  modulation), SBNK instruments (single, drum set, key split), PCM8/PCM16/IMA-ADPCM samples, PSG square and noise
  channels, ADSR envelopes, 16-voice mixer with priority-based voice stealing
- Streams decode one block at a time from the SD card, so big ROMs never need to fit in RAM
- Songs fade out after 2 loops, or use the `length`/`fade` tags in NCSF files; repeat mode loops forever
- Auto-advances through the song list or folder

## Controls

| Button | Action |
|---|---|
| D-pad Up/Down | Move cursor |
| D-pad Left/Right | Page up/down |
| Circle Pad Up/Down, ZL/ZR | Volume down/up (10–400%, saved) |
| A | Open folder / open ROM / play |
| B | Back (song list → files, or parent folder) |
| L / R | Previous / next song |
| Y | Pause / resume |
| X | Stop |
| SELECT | Repeat current song on/off |
| START | Quit |

The last folder you were in and your volume are remembered in `sdmc:/3ds/ndsmusicplayer/`. Volume starts at 150%;
100% is the DS's own level, and anything above that is boosted with a soft limiter so loud parts don't distort.

## DSE games and 2SF sets

Explorers of Sky (and Time/Darkness) don't use Nintendo's sound archive; their music is in `SOUND/BGM` in DSE format.
The app has a separate DSE player for these:

- **ROMs**: open the `.nds` as usual; songs are listed as `NNN NAME` (e.g. `159 B_MAP_P3_CAFE`).
- **2SF sets** (`.mini2sf` + `.2sflib`): open any `.mini2sf`. The first time, the app streams through the
  `.2sflib` once and keeps only the music files (about 4 MB out of the 128 MB game image), which takes a few
  seconds; after that every song in the folder starts immediately. Each `.mini2sf` stores its song number, so the
  game's code never has to run. Only DSE-based 2SF sets work this way; 2SF sets of other games are not supported.

The DSE player was checked against the game's own sound engine running in an emulator: note timing, pitch
(within 0.02 semitones), envelopes and volume (within about 1 dB) match closely. Instrument vibrato is supported;
the rarely used volume/pan LFOs and a few slide commands are not.

## Installing

1. Download `ndsmusicplayer.3dsx` from the [latest release](https://github.com/nibbles517/nitroplayer3ds/releases/latest)
   and copy it to `sdmc:/3ds/`.
2. Make sure `sdmc:/3ds/dspfirm.cdc` exists (run **DSP1** once if you've never done it). Without it, there's no sound.
3. Put your ROM dumps and NCSF files anywhere on the SD card and launch the app from the Homebrew Launcher.

## Building

### GitHub Actions (no local setup)

Every push runs `.github/workflows/build.yml`, which builds the `.3dsx` inside the official `devkitpro/devkitarm`
container and attaches it to the run as an artifact (**Actions** tab → latest run → *ndsmusicplayer-3dsx*). It also
builds the PC tools and renders the synthetic test songs with sanitizers on.

### Locally

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group, then from this folder:

```sh
make
```

On Windows, run that from the devkitPro MSYS2 shell.

## Project layout

```
source/            3DS front end
  main.c           UI: file browser, song list, now-playing screen
  audio.c          ndsp output thread
  browser.c        SD card directory listing
source/core/       portable C99 player core (no 3DS dependencies)
  datasource.c     memory / FILE* random-access reader
  nds_rom.c        NDS header + NitroFS walker, SDAT discovery
  nds_music.c      open a ROM, find and parse its SDATs
  nds_banner.c     DS Menu icon (static + DSi animated) and titles
  sdat.c           SDAT parser (INFO/SYMB/FAT), loads a song's SSEQ+SBNK+SWARs
  wave.c           SWAV/SWAR decoding (PCM8, PCM16, IMA-ADPCM)
  sseq.c           sequencer, instruments, envelopes, LFO, mixer
  strm.c           streamed audio
  ncsf.c           NCSF/miniNCSF loader and tags
  dse.c            DSE (Procyon) sequencer, instruments, envelopes and mixer
  dse_set.c        finds a DSE game's songs and sample banks
  twosf.c          2SF sets of DSE games: streams the .2sflib, reads song numbers
  inflate.c        small zlib decompressor (whole-buffer or streaming)
  song.c           one playable song + loop/length/fade handling
tools/
  render_wav.c     render any song to a .wav on PC using the same core
  sdat_list.c      list a ROM's SDATs, sequences and streams; dump a song's files
  banner_dump.c    print a ROM's banner titles/animation and save each icon frame as .ppm
  make_music_rom.py  builds a synthetic test ROM + NCSF (no game data)
  make_test_rom.py   builds minimal ROMs for parser tests
```

## Using the player in other projects

Everything under `source/core/` is plain C99 with no 3DS dependencies, so it can be dropped into other homebrew
(game ports in particular) to play a game's own DS music. [pmd-red-3ds](https://github.com/nibbles517/pmd-red-3ds)
uses it this way. `source/audio.c` shows how to feed it to the 3DS's sound hardware with ndsp.

## PC tools

From the repo root (any C99 compiler; on Windows use MSYS2/MinGW):

```sh
cc -std=c99 -O2 -Isource/core tools/render_wav.c source/core/[a-z]*.c -lm -o render_wav
cc -std=c99 -O2 -Isource/core tools/sdat_list.c  source/core/[a-z]*.c -lm -o sdat_list
cc -std=c99 -O2 -Isource/core tools/banner_dump.c source/core/[a-z]*.c -lm -o banner_dump

./sdat_list game.nds                       # list songs
./render_wav game.nds out.wav -q 12        # render sequence 12
./render_wav game.nds out.wav -t 0         # render stream 0
./render_wav game.nds out.wav -a 2 -q 15   # render sound effect 15 of archive 2
./render_wav song.minincsf out.wav         # render an NCSF
./render_wav song.mini2sf out.wav          # render a 2SF song of a DSE game
./render_wav eos.nds out.wav -b 159        # render DSE song bgm0159 from a ROM
./banner_dump game.nds icon_                # titles + icon frames (icon_0.ppm, ...)
```

## Limitations

- Games that don't use Nintendo's standard sound system (some third-party titles) have no SDAT and show
  "No SDAT found".
- Some effects are meant to be steered by the game while they play (engine pitch, loops the game stops itself).
  On their own they loop briefly and fade out, or sound plainer than in the game.
- The mixer follows the DS SDK's volume, envelope and pitch curves, but it isn't a cycle-accurate emulation, so
  some songs may sound slightly different from real hardware.
