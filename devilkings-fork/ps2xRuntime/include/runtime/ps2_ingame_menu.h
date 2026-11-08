#pragma once
// LOCAL PATCH: the in-game menu. The launcher's own page (launcher_assets/launcher.html, from the Claude Design file) shown
// over the game in Microsoft Edge WebView2, in its in-game mode: Resume / Configure Controller / Mod Menu / Quit, the same
// graphics settings (applied live where the runtime can, else marked "restart"), controller navigation. Opened with F1 or
// Select+Start (ps2_runtime.cpp). Window thread only. No Windows or raylib types here (windows.h clashes with raylib).
namespace ps2x::ingame
{
    enum class Request
    {
        None,
        Quit,    // Quit Game
        Restart, // Restart Game: start the game again with the saved settings, then quit this one
    };

    // `window` is the game window's HWND (raylib GetWindowHandle()).
    void open(void *window);
    void close();
    [[nodiscard]] bool isOpen();
    // Every frame: keeps the panel centred and sized to the window. Cheap when closed.
    void update(void *window);
    [[nodiscard]] Request takeRequest();
    // Starts a second copy of this game process with the same command line and folder (for Restart).
    bool relaunchGame();
}
