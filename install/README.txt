AI Limit VR
VR mod for AI LIMIT (Steam).
Native stereo with the headset at the game's third person camera, played seated with a gamepad.
This mod is free. If you enjoy it, you can buy me a coffee: https://buymeacoffee.com/scubasteve2365

You need
- The game on Steam.
- A PC VR headset with an OpenXR runtime. Tested: SteamVR (set as your OpenXR runtime in SteamVR settings) on a Pimax Dream Air, and Virtual Desktop's VDXR on a Meta Quest 3.
- An Xbox type gamepad.
- For DLSS: an NVIDIA RTX graphics card. Others can use TAA in the mod menu.

Install
1. Unzip this folder anywhere.
2. Double click "Install AILimitVR.bat" and type 1.
If another mod already uses winhttp.dll in the game folder (BepInEx does), the installer renames it to winhttp_chain.dll and this mod loads it first.

Play
1. Start SteamVR or connect Virtual Desktop.
2. Start the game from Steam. VR starts by itself and the view centres itself after a second.
3. Load your save and play.

Settings
Hold both sticks in for half a second to open the VR settings menu, or press Insert on the keyboard. The game gets no input while it is open. LB and RB (Page Up and Page Down) switch tabs, up and down choose a setting, left and right change it, A (Enter) selects, B (Esc) closes. Every row explains itself at the bottom of the menu.
General
- Recenter: puts the view, the HUD and the menu in front of you again. Your headset's own recenter does the same.
- World scale: 1.00 is life size. Higher makes the world smaller with more 3D depth.
- Decoupled pitch: On keeps the world level; look up and down with your head. Off tilts the view with the game's camera.
- Upscaler: TAA, DLAA, or DLSS Quality, Balanced (default), Performance, Ultra Performance, in each eye.
- Resolution: 100% is the size your headset asks for. Takes effect the next time you start the game.
- Picture: Auto (recommended) uses Steady on Virtual Desktop and Panel on SteamVR. Steady draws the HUD and menu into the 3D picture, which holds still on Virtual Desktop. Panel shows them as separate sharp panels, best on SteamVR.
HUD
- HUD mode: Fixed (stays in the room), Follows head, or Hidden. The game's full screen menus show on the same panel.
- HUD size, left / right, height, distance, and Lock-on marker height.
Mod Menu
- Menu size, height, distance, and Menu sounds (Off, Low, Medium, High).
Game
- Load VR baseline settings: the game's default quality with VSync, frame lock, motion blur and ambient occlusion off.
- Load best VR performance: lowest shadows and fog, ambient occlusion and motion blur off, DLSS on.
Both buttons ask for a second press.

While VR runs the mod keeps the game's VSync and frame lock off (it paces the game to your headset) and turns off film grain, bloom, motion blur, radial blur and ambient occlusion, which look wrong in VR.

Uninstall
Double click "Install AILimitVR.bat" and type 2. No game files are changed by this mod.

Performance tips
- SteamVR: in Settings, Video, Per-application video settings, set the game's Throttling Behavior to Fixed at your refresh rate, and Motion Smoothing off.
- DLSS Balanced holds 72 Hz on an RTX 5090 at a high SteamVR resolution. Lower the Upscaler step or the Resolution if your card struggles.
- Game speed is correct at 72, 90 and 120 Hz.

Good to know
- The mod keeps the game offline: it refuses the game's internet connections.
- Your monitor shows nothing while VR runs; the game's flat picture is skipped to save time.
- No motion controls. PlayStation pads work through Steam Input (Steam, the game's Properties, Controller, Enable Steam Input).

If something goes wrong
- The game stays flat: a message box says why VR did not start, with what to do. Check your OpenXR runtime is set (SteamVR or Virtual Desktop). Details are in AILimitVR.log in the game folder.
- Windows warns about the bat file: the mod is not signed, so Windows may ask you to confirm. The bat is plain text; read it first if you like.
- Smart App Control: if it is on, Windows can block unsigned mod DLLs and the game runs flat.
- Please attach AILimitVR.log (and any files in the AILimitVR\crashes folder) when you report a problem.

Tested on
A Pimax Dream Air through SteamVR and a Meta Quest 3 through Virtual Desktop's VDXR, NVIDIA RTX graphics. Other headsets should work through OpenXR but have not been tried. AMD and Intel graphics have not been tried.

Support: https://buymeacoffee.com/scubasteve2365

Version 1.0. Credits and licences are in THIRD-PARTY-NOTICES.txt. MIT licensed, by scubasteve2365.
This is a fan mod, not made or approved by Sense Games or CE-Asia.
