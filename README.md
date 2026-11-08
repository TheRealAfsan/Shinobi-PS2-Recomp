# Shinobi (PS2) Recomp

## Download and play

1. Click the green **Code** button above, then **Download ZIP**. When it has downloaded, right-click the file and choose **Extract All...** (any folder except Program Files). That is GitHub's own download, the only unzipping there is.
2. Run **`Shinobi Launcher.exe`** (it is in the main folder you just unzipped).
3. Press **Game File**, choose your own Shinobi (USA) disc image, press **Set up game files**, then **Play Game**.

Everything is in this one repository: the launcher, the game program and the source code.

> ### You must own the game
> Buy your own copy of **Shinobi (PlayStation 2, USA)** and make a disc image (`.iso`) from your own disc with a DVD drive and a disc-imaging tool. **This repository contains no game, no disc image and no game data**, and it never will. We do not provide, link to or help anyone find a copy of the game. No disc, no game.

---

A native Windows port of **Shinobi** (PlayStation 2, USA) by **TheRealAfsan**, [Twilight Orchard](https://www.youtube.com/@TheReal_Afsan).

YouTube: <https://www.youtube.com/@TheReal_Afsan>  
Reddit: <https://www.reddit.com/user/Afsan23/>

> **This repository does not contain the game.** No disc image, no game files and no game data. The launcher's artwork and music are the author's own. You need your own copy of Shinobi (PS2, USA, SLUS-20459), made into an image file from the disc you own.

## What this is

The game's own program is translated ("recompiled") to run natively on Windows. PCSX2's graphics (GS) and sound (SPU2) code is used for the parts that have to match the PS2 exactly. A launcher lets you pick your game file, set resolution, frame rate and controls, and press Play.

## What you need

- Windows 10 or 11, 64-bit
- A graphics card with Direct3D 11 (default) or Vulkan
- Microsoft Edge WebView2 Runtime (part of Windows 11)
- The Microsoft Visual C++ Redistributable (x64), which most PCs already have
- Your own disc image of Shinobi (USA), `.iso` (or `.bin` / `.img` with raw sectors)
- About 1.5 GB free space, plus saves

Developed on a Ryzen 7 9800X3D and a GeForce RTX 5080. The CPU is the main limit.

## Quick start

1. Start `Shinobi Launcher.exe`.
2. **Game File → Browse...** and choose your Shinobi (USA) disc image. The launcher checks it is the right game.
3. **Set up game files** copies the game's files out of your image into the `game` folder (about 1.2 GB, once).
4. **Play Game (Recom).**

The launcher remembers where your game file is and shows it under the Game File button.

## Controls

Everything can be changed in **Configure Controller** (and the in-game menu, F1).

| | Keyboard default |
|---|---|
| D-pad | Arrows or W A S D |
| Cross / Circle | X or Space / C or Escape |
| Square / Triangle | Z or Numpad 0 / V or Numpad 1 |
| L1 / R1, L2 / R2 | Q / E, Left Shift / Right Shift |
| Start / Select | Enter / Tab |
| Camera (right stick) | I K J L |

- **Mouse:** moves the camera; left click = Square, right click = Circle; **F10** switches it on and off.
- **Controllers:** DualSense, DualSense Edge, DualShock 4 and Xbox (360, One, Series) use the standard layout. Only the DualSense Edge is tested so far.
- **In game:** F1 or Select+Start = menu (pauses), Alt+Enter or F11 = window / full screen, F8 = mark a problem in the log.

## Saves

Saves are kept in the `saves` folder next to the launcher, apart from the game files: `saves/mc0`, `saves/mc1`, and dated copies in `saves/backups`. Copy that folder to back up or move your saves.

## Sound note

The game's streamed music can reach the sound chip with a damaged right channel (static in the right ear, a late copy of the sound). The cause is still being tracked down. Until then `audio.stereo_repair` in `game/settings.ini` plays the clean left channel in both ears (`copy_left`, default); sound effects stay in stereo. Set it to `stereo` to turn the repair off.

## Known issues

- **21:9 widescreen is experimental:** a pink square can appear when an enemy dies, and the game can crash. 4:3 and 16:9 (with or without the widescreen hack) are fine. Use 16:9 on an ultra-wide screen for now.
- Frame rate can dip in the heaviest fights.
- Music is the same in both ears by default (see the sound note).
- Screen effects (fog, glow, blur) at high resolutions are not fully checked.
- Controllers other than the DualSense Edge are untested; the mouse buttons are guessed attack buttons.

## Building from source

The source is GPL-3.0 (see `LICENSE`). The source repository does not contain the translated game code: it is generated from the game's own program file. The downloadable release contains it only inside the finished program (`bin/Shinobi.zip`, unpacked by the launcher by itself); it holds no game data. A step-by-step build guide is still to come. The runtime is in `devilkings-fork/ps2xRuntime`.

## Credits

- **Shinobi (PS2) Recomp:** TheRealAfsan, Twilight Orchard
- **PS2Recomp:** [ran-j](https://github.com/ran-j/PS2Recomp) and contributors
- **PCSX2** (graphics and sound code, GPL): the PCSX2 Team, <https://pcsx2.net>
- **raylib:** Ramon Santamaria and contributors, <https://www.raylib.com>
- **Microsoft Edge WebView2** shows the launcher page

See `THIRD_PARTY_NOTICES.txt`.

## Legal

An unofficial fan project, not made by, affiliated with or endorsed by SEGA or Sony. Shinobi and SEGA are trademarks of SEGA Corporation; PlayStation is a trademark of Sony Interactive Entertainment. You need your own legally obtained copy of the game. Free software under the GNU General Public License v3.0, with no warranty.
