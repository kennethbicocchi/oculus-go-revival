// KWin script (run by pc/vr_pointer.py over D-Bus): give the keyboard focus to the running Steam
// game, whose window class is steam_app_<appid>. Games ignore the gamepad while unfocused, and
// a game started from the SteamVR dashboard or the GoVR panel does not get the focus by itself.
const wins = workspace.windowList ? workspace.windowList() : workspace.clientList();
for (const w of wins) {
    if ((w.resourceClass || "").indexOf("steam_app_") === 0 && (w.resourceClass || "") !== "steam_app_0") {
        workspace.activeWindow = w;
    }
}
