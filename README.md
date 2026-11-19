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

## Using the player in other projects

Everything under `source/core/` is plain C99 with no 3DS dependencies, so it can be dropped into other homebrew
(game ports in particular) to play a game's own DS music. [pmd-red-3ds](https://github.com/nibbles517/pmd-red-3ds)
uses it this way. `source/audio.c` shows how to feed it to the 3DS's sound hardware with ndsp.

## Limitations

- Games that don't use Nintendo's standard sound system (some third-party titles) have no SDAT and show
  "No SDAT found".
- Some effects are meant to be steered by the game while they play (engine pitch, loops the game stops itself).
  On their own they loop briefly and fade out, or sound plainer than in the game.
- The mixer follows the DS SDK's volume, envelope and pitch curves, but it isn't a cycle-accurate emulation, so
  some songs may sound slightly different from real hardware.
