==============================================================================
  SHINOBI (PS2) RECOMP
  A native Windows port of Shinobi (PlayStation 2, USA)
  by TheRealAfsan  -  Twilight Orchard
==============================================================================

  YouTube : https://www.youtube.com/@TheReal_Afsan
  Reddit  : https://www.reddit.com/user/Afsan23/

  >>> TO PLAY:  run  Shinobi Launcher.exe  (in this folder)
      Then GAME FILE -> BROWSE (your own Shinobi USA disc image) -> SET UP
      GAME FILES -> PLAY GAME.   Details are in section 3 below.


------------------------------------------------------------------------------
  WHAT'S NEW IN v0.4
------------------------------------------------------------------------------

  - DualSense / DualSense Edge HD haptics, USB and Bluetooth: footsteps left and
    right, shuriken throws, sword hits, dash, the shuriken storm, ninja magic.
    L2 adaptive trigger: it clicks through just before the magic fires.
  - Close-up, over-the-shoulder camera (F3 in game).
  - Performance overlay: FPS, frame time, 1% low, CPU, GPU, RAM, VRAM (F2).
  - 8K resolution; most settings now change live in the F1 menu.
  - New graphics options: blending accuracy, texture filtering, screen filter
    (sharp / smooth / scanlines / CRT), dithering, mipmapping, upscaling fixes,
    window mode (borderless / exclusive fullscreen / windowed).
  - Faster: geometry on several CPU cores, a lighter sound chip, Low-end PC preset.
  - Hotsuma's scarf in any colour (Mod Menu).
  - The original game manual in the launcher; settings in tabs; the launcher is
    fully usable with a controller and its window resizes and snaps.

  IN GAME:  F1 menu   F2 performance overlay   F3 close-up camera   F4 manual
            Alt+Enter / F11 window or full screen


------------------------------------------------------------------------------
  1. WHAT THIS IS
------------------------------------------------------------------------------

Shinobi (PS2 Recom) runs the PlayStation 2 game Shinobi as a normal Windows
program. It is not an emulator of the whole console: the game's own program was
translated ("recompiled") to run natively, and it uses PCSX2's graphics and
sound code for the parts that must match the PS2 exactly.

It comes with a launcher (Shinobi Launcher.exe) where you pick your game file,
set the resolution, frame rate and controls, and press Play.

YOU MUST OWN THE GAME. Buy your own copy of Shinobi (PlayStation 2, USA) and
make a disc image (.iso) from your own disc with a DVD drive and a disc-imaging
tool. No disc, no game.

THIS PROJECT DOES NOT CONTAIN THE GAME. No game files, no disc image, no
game data, artwork or music of the game are included. The launcher's own
artwork and music are by the author. You need your own copy of
Shinobi (PlayStation 2, USA) - the disc you own, made into an image file.


------------------------------------------------------------------------------
  2. WHAT YOU NEED
------------------------------------------------------------------------------

  - Windows 10 or 11, 64-bit.
  - A processor with AVX2: Intel Core 4th generation (2013) or newer, or AMD
    Ryzen. Celeron, Pentium and Atom chips usually do not have it.
  - A graphics card that supports Direct3D 11 (default) or Vulkan.
  - Microsoft Edge WebView2 Runtime for the launcher (already part of
    Windows 11; Windows 10 users can get it free from microsoft.com).
  - The Microsoft Visual C++ Redistributable (x64), which most PCs already
    have. If the game does not start, install it free from microsoft.com.
  - YOUR OWN disc image of Shinobi (PS2, USA, game ID SLUS-20459) as an
    .iso file (a .bin or .img with raw sectors also works).
  - About 1.5 GB of free space for the game's files, plus room for saves.

It was developed and tested on a Ryzen 7 9800X3D with a GeForce RTX 5080. The
processor is the main limit; a fast CPU matters more than a fast graphics card.


------------------------------------------------------------------------------
  3. QUICK START
------------------------------------------------------------------------------

  1. Start  Shinobi Launcher.exe
  2. Press  GAME FILE, then  BROWSE...  and choose your Shinobi (USA) disc
     image. The launcher checks that it really is Shinobi (USA) and tells you
     if it is a different game.
  3. Press  SET UP GAME FILES. The game's files are copied out of your disc
     image into the "game" folder (about 1.2 GB, done once).
  4. Close the window and press  PLAY GAME (RECOM).

The launcher remembers where your game file is, and shows it under the Game
File button every time you start it.


------------------------------------------------------------------------------
  4. CONTROLS
------------------------------------------------------------------------------

Everything can be changed in the launcher under CONFIGURE CONTROLLER (and in
the in-game menu, F1).

  KEYBOARD (default)
    D-pad ........ Arrow keys or W A S D
    Cross ........ X or Space           Circle ....... C or Escape
    Square ....... Z or Numpad 0        Triangle ..... V or Numpad 1
    L1 / R1 ...... Q / E                L2 / R2 ...... Left Shift / Right Shift
    Start ........ Enter                Select ....... Tab
    CAMERA (right stick): I = up, K = down, J = left, L = right

  MOUSE
    Moving the mouse turns the camera. Left click = Square (attack),
    right click = Circle. The pointer stays inside the game window.
    F10 switches the mouse camera on and off.

  CONTROLLER
    Plug in a gamepad before starting the game. PlayStation (DualSense,
    DualSense Edge, DualShock 4) and Xbox (360, One, Series) controllers use the
    standard layout: the bottom face button is Cross, the right one is Circle,
    and so on. The sticks have a dead zone so a resting stick does not drift.
    Only the DualSense Edge has been tested by the author.

  DURING THE GAME
    F1 or Select+Start ... in-game menu (pauses the game)
    Alt+Enter or F11 ..... window / full screen
    F8 ................... mark a problem in the log (for bug reports)


------------------------------------------------------------------------------
  5. SETTINGS
------------------------------------------------------------------------------

  Resolution ....... 1x to 6x the PS2's own picture (720p, 1080p, 1440p, 4K)
  Renderer ......... Direct3D 11 (default) or Vulkan
  Frame rate ....... 60 (the game's own speed), 30, 50, or unlocked
  Widescreen ....... 16:9, 21:9 or 32:9 (a picture hack; 4:3 is the default)
  V-Sync, Sharpening, Anti-aliasing (FXAA or Edge AA), Texture quality
  Mod Menu ......... studio intro, skip logos, full screen, diagnostics and
                     a few more switches

Settings are saved in  game\settings.ini.  Most apply when you press Save;
the in-game menu changes most of them live.


------------------------------------------------------------------------------
  6. WHERE YOUR SAVES ARE
------------------------------------------------------------------------------

Saves are kept in the  saves  folder next to the launcher - apart from the
game files and not tied to your disc image:

    saves\mc0       memory card 1 (your Shinobi saves)
    saves\mc1       memory card 2
    saves\backups   a dated copy of both cards, made each time the game starts

You can open it from the launcher: GAME FILE, then OPEN SAVES FOLDER. To move
your saves to another PC or back them up, copy the saves folder.


------------------------------------------------------------------------------
  7. SOUND NOTE
------------------------------------------------------------------------------

The game's streamed music (the cutscenes and the music during play) can reach
the sound chip with a damaged right channel: static in the right ear and a
late, out-of-sync copy of the sound. The cause is still being tracked down.

Until it is found, the setting  audio.stereo_repair  in game\settings.ini
plays the music's clean left channel in both ears ("copy_left", the default).
Sound effects keep their full stereo. Set it to "stereo" to turn the repair
off and hear the music as the game sends it.


------------------------------------------------------------------------------
  8. KNOWN ISSUES
------------------------------------------------------------------------------

  - 21:9 widescreen is experimental: a pink square can appear when an enemy
    dies, and the game can crash. 4:3 and 16:9 are fine; use 16:9 on an
    ultra-wide screen for now.
  - Frame rate can dip in the heaviest fights (the CPU is the limit).
  - Music is mono-in-both-ears by default (see SOUND NOTE).
  - Screen effects (fog, glow, blur) at high resolutions are not fully checked.
  - The first seconds after starting (logos, intro, loading) run slower.
  - Controllers other than the DualSense Edge are untested.
  - The left and right mouse buttons are guessed attack buttons.

Found a bug? Press F8 in the game at the moment it happens, then describe it
with the log in  game\captures\last_run.log.


------------------------------------------------------------------------------
  9. BUILDING FROM SOURCE
------------------------------------------------------------------------------

The source code is released under the GPL-3.0 (see LICENSE). The source
repository does not contain the translated game code: that is generated from
the game's own program file. The downloadable release contains it only inside
the finished program (bin\Shinobi.zip, unpacked by the launcher by itself); it holds no game data. A step-by-step
build guide is still to come; the runtime lives in  devilkings-fork\ps2xRuntime.


------------------------------------------------------------------------------
  10. CREDITS
------------------------------------------------------------------------------

  Shinobi (PS2) Recomp - project, port, launcher, testing
      TheRealAfsan, Twilight Orchard

  PS2Recomp - the PlayStation 2 static recompiler this port builds on
      ran-j and contributors   https://github.com/ran-j/PS2Recomp

  PCSX2 - graphics (GS) and sound (SPU2) code, used under the GPL
      The PCSX2 Team            https://pcsx2.net

  raylib - window, input and audio
      Ramon Santamaria (raysan5) and contributors   https://www.raylib.com

  Microsoft Edge WebView2 - shows the launcher page

  Shinobi (c) SEGA, developed by Overworks. All rights belong to their owners.


------------------------------------------------------------------------------
  11. LEGAL
------------------------------------------------------------------------------

This is an unofficial fan project. It is not made by, affiliated with or
endorsed by SEGA or Sony. "Shinobi" and SEGA are trademarks of SEGA
Corporation; "PlayStation" is a trademark of Sony Interactive Entertainment.

You need your own legally obtained copy of the game. Nothing here helps you
get a copy of the game, and the project will not accept links to one.

The program is free software under the GNU General Public License v3.0 (see
the LICENSE file). It comes with NO WARRANTY.

==============================================================================
