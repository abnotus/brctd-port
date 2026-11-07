# Countdown to Doomsday, natively

Play *Buck Rogers: Countdown to Doomsday* (SSI, 1990) natively on
Windows and Linux. This is a new engine that reads **your own copy** of
the DOS game and plays it as the original did on a VGA PC with an AdLib
card. The rules, maps, story and fights are the original's.

**No game files are included.** You need your own copy of the DOS game,
version 1.00. Nothing else. (The one thing built in is the Log Book's
text, encrypted so that only your own game files open it: see "The Log
Book" below.)

Downloads, source and bug reports: <https://github.com/abnotus/brctd-port>
(report problems under Issues).

**Runs on** 64-bit Windows 10 or 11, and 64-bit Linux (x86-64) with
glibc 2.29 or newer (most distributions from 2020 on).

## Getting started

1. Unpack the download into a folder of its own.
2. Copy all of your game's files (START.EXE, GAME.OVR, ITEMS, the .DAX
   files and the rest, as installed for DOS) into the `game` folder beside
   `ctd` (Linux) or `ctd.exe` (Windows). Upper or lower case names both
   work.
3. Start `ctd`: on Windows double-click `ctd.exe`; on Linux run `./ctd`
   in a terminal in that folder (or make your file manager run it).

If something is missing or wrong, `ctd` tells you what and where.

**Windows:** the first start may show "Windows protected your PC". The
program isn't signed. Click **More info**, then **Run anyway**. Windows
won't ask again.

## Which copy works

DOS version 1.00. These two files must match exactly:

| File      | Size          | SHA-256                                                            |
|-----------|---------------|--------------------------------------------------------------------|
| START.EXE | 67,619 bytes  | `0d640044c3795f2826322c64ca422bd225571f5dc62f3a26460ca9c7d9afa5ee` |
| GAME.OVR  | 210,071 bytes | `3a4ad4856c08fe5973179f1d907feed1d870af99d08abd1cb884b316324f3cc0` |

With another version, `ctd` stops, says which files differ, and writes a
`ctd-hashes.txt` (file names, sizes and hashes only; it tells you
where). Send that file with your report and your version may be added
later.

## Your saves

Your game folder is only read, never changed. `ctd` keeps its own
folders beside the program:

- `saves`: your saved games. On the very first start, the saves in your
  game's `SAVE` folder are copied in, so you can carry on where you left
  off (copy them into `saves` yourself after that).
- `ctd-logs`: one file per session, recording your key presses. When you
  report a problem (on the project's Issues page), include the newest one.

If that folder can't be written to, they go to `%APPDATA%\ctd`
(Windows) or `~/.local/share/ctd` (Linux) instead.

Your options are kept in `settings.ini` in that same user folder
(`%APPDATA%\ctd` or `~/.local/share/ctd`); delete it to reset them.

## Keys

The game's own keys work as in DOS. Extra keys for the window:

| Key              | Does                                           |
|------------------|------------------------------------------------|
| F11 or Alt+Enter | Full screen on/off                             |
| Shift+F11        | 4:3 aspect correction on/off                   |
| F12              | Combat speed                                   |
| Shift+F12        | Options screen (also Options in the team menu) |
| F1               | At a passcode prompt: types the code           |

The options screen remembers your choices. OPTIONS.md lists them all.

## What's different from the original

- **No setup questions.** Graphics, sound and controls are set to the
  best the original supports.
- **Options replace JOYSTICK/MOUSE INITIALIZE** in the team menu;
  Messages and Anim from CHANGE are there too.
- **Full screen and 4:3 shape**; closing the window asks first.
- **Text appears at once** (the Text style option brings back
  letter-by-letter). Keys pressed while text is still appearing are
  ignored, so you can't answer a question by accident.
- **Combat speed** (F12), and a short pause for each step a
  computer-controlled fighter takes, so you can follow it.
- **Sound effects** sound as on a Tandy 1000's sound chip, even with the
  AdLib music; the original then used the PC speaker. (AdLib versions
  of the effects are an option.)
- **The Log Book** is built in (see below).
- **Gamepads** work.
- **Saves** stay beside `ctd`, never in the game folder.
- **Three bugs fixed:** stashed items are no longer lost on loading; an
  emptied stash no longer comes back; characters a load or save would
  have deleted are moved to `saves/DELETED` instead.
- **Names** can't contain characters a file name can't hold, so every
  character can be saved.
- **Damaged saves** are refused with a message, and the few spots where
  the original would freeze carry on or restart instead.

## The Log Book

When the game sends you to an entry in the printed Log Book, `ctd` shows
that entry on screen, in the game's own text window. The book's two maps
are shown too.

The text is built in and encrypted. It unlocks only with your own
START.EXE and GAME.OVR, and is never written to disk.

At a passcode prompt, press **F1** to type the code.

## Gamepads

A gamepad acts as the keyboard:

| Button            | Key                                           |
|-------------------|-----------------------------------------------|
| A, Start          | Enter                                         |
| B, Back           | Esc                                           |
| X                 | Space                                         |
| Y                 | Y (at a passcode prompt: types the code)      |
| D-pad, left stick | Arrows (stick corners: Home, PgUp, End, PgDn) |
| Shoulders         | PgUp, PgDn                                    |
| Stick buttons     | Home, End                                     |
| Guide             | Options screen                                |

At name and number prompts, Up and Down change the letter, Right moves
on, Left deletes. The options screen lets you give any button or
trigger a different key.

Tested with an Xbox controller on Windows. Gamepads on Linux are
untested so far.

## Building

`ctd version` shows which build you have. To build it yourself, get the
source from <https://github.com/abnotus/brctd-port> and see its
BUILDING.md.

## Licence

Copyright (C) 2026 Abnotus. GPL-3.0-or-later (LICENSE). SDL3 is under
the zlib licence and Nuked-OPL3 under the LGPL-2.1-or-later
(THIRD_PARTY.md; the downloads hold every licence in their `licenses`
folder).

*Buck Rogers* and *Countdown to Doomsday* are trademarks of their
respective owners. This project is not affiliated with or endorsed by
them or by SSI.
