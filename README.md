# Voodoo Vince Community Patch

One patch for **Voodoo Vince Remastered** that puts the community fixes together: graphics, sound, frame pacing,
crashes and a few look-and-feel touches. An installer applies them, lets you pick three optional parts, change your mind
later, and put the game back exactly as it was.

It needs the **Steam** version of Voodoo Vince Remastered, **1.14.2.0** (the current one), on Windows 10 or 11.
Any other game version is refused before anything is written.

## What it fixes

Always installed:

- **High DPI fix:** the game runs at your screen's real resolution when Windows scaling is above 100 %.
- **CPU thread fix:** the game's threads are no longer pinned to the first CPU cores.
- **60 fps cap with an improved frame limiter:** steadier frame pacing (settings in `VinceAIO.ini`, see below).
- **Audio fixes:** stereo fix; no more lost sound when the audio device changes (no-audio and audio-cutoff fixes);
  audio device caching; the power icon pickup sound plays again; the volume is adjusted (MasterVolume 0.5).
- **Loud fan noise in Climbing Kosmobot** turned down.
- **Land o' Rides cat noise** turned down.
- **Charmaine's voice and subtitle** fixed.
- **Cutscene crash when skipping** fixed.
- **Field of view of the butler ghost** fixed.
- **Dino boss (Bone Goliath) lightning** fixed.
- **Vince's eye animation in the Janice cutscene** fixed. Janice pupil fix by **Chameleon2606**,
  included with permission.
- **Camera near plane** adjusted so surfaces that sit very close together, like Vince's pupils, no longer flicker
  when seen from far away.
- **Crypt City lights glow again**, and the urns show their reflections.
- **Water** shader and visuals restored, including the Main Tunnel puddle (ripples and splashes).
- **Lightmap fix** in the bog.
- **Entering a warp:** Vince walks out again.
- **Heat shimmer** over the propeller room steam and the lantern fire.
- **Kosmo's cabin smoke** and the Crypt City gate fog draw correctly.
- **Bumper car** fog increased, and the bumper car obstacles animate.
- **Ultrawide:** 21:9 and 32:9 screens fill without black bars; the HUD and menus stay 16:9 in the middle.
  By **Chameleon2606**, included with permission. If you change the resolution or display mode while playing, the
  picture keeps its old shape until you go back to the main menu, which puts it right.
- **Particles, sparkles and glows** have the right shape again (they were drawn stretched). By **Chameleon2606**,
  part of the ultrawide fix.
- **Zombie Guidance Counselor ball** has a toned-down look.

Optional (the installer asks about each one):

| Option name | What it does |
| --- | --- |
| `voodoopowerfix` | New materials for the power icon, the power skull and the beads. |
| `firebug` | New material for the firebug's shell. |
| `retexture` | Updated Vince texture and normal map. Vince retexture by **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**, included with permission. |

## Install

1. **Close the game.**
2. **Unzip** `VoodooVinceCommunityPatch-2.0.0.zip` into a folder of its own, for example
   `Downloads\VoodooVinceCommunityPatch`. Not into the game folder.
   **Keep this folder** after installing: you need it to change options or uninstall.
3. Open the folder, then the `installer` folder, and **double-click `Install.cmd`**.
4. A black window opens. It finds your Steam game by itself and shows what it sees, for example:

   ```
   Game folder: C:\Program Files (x86)\Steam\steamapps\common\Voodoo Vince Remastered
   Package: Voodoo Vince Community Patch 2.0.0
   Vince.exe: stock Steam build
   State: not installed
   Choose: [I]nstall, [C]hange options, [U]ninstall, [Q]uit:
   ```

   If you have the game in several Steam libraries it lists them and asks `Install number:`. If it finds none it asks
   `Game folder (the one containing Vince.exe):`; paste the folder (in Steam: right-click the game, *Manage*,
   *Browse local files*).
5. Type `I` and press Enter. It then asks about the three optional parts, one at a time:

   ```
   Enable voodoopowerfix? (y/n)
   Enable firebug? (y/n)
   Enable retexture? (y/n)
   ```

   Answer `y` for the ones you want and `n` for the others (the table above says what each does).
6. It says `Working; this takes a moment.`, checks every file, then writes them, listing each one as `[1/...]`,
   `[2/...]` and so on. Keep the window open until the end, where it says `Installed set ... (options: ...)`. Press
   any key to close the window, and play.

### "Windows protected your PC"

The patch is not signed with a paid certificate, and the zip came from the internet, so Windows may show a blue
**SmartScreen** box when you start `Install.cmd`. Click **More info**, then **Run anyway**. If your PC has
**Smart App Control** turned on it may block it with no such button. In both cases you can remove the "downloaded from
the internet" mark first: right-click the **zip**, choose *Properties*, tick **Unblock**, click OK, and unzip again.

The installer is a plain-text PowerShell script (`installer\VinceAIO.ps1`) that you can read. It does not need
administrator rights, does not go online and does not touch the registry. It writes only inside the game folder.

## Change options

Run `installer\Install.cmd` again, type `C`, and answer the three questions again. Only the files that differ between
the old and the new choice are changed.

## Check what is installed

Run `installer\Install.cmd`. The first lines always show the state: `not installed`, or `installed set ...` with the
options you picked. Type `Q` (or just press Enter) to leave without changing anything; it says `Nothing changed.`

## Uninstall

Run `installer\Install.cmd`, type `U`. It puts every file back and says
`Uninstalled: the game files are back to stock.` (or `... back to how they were before Voodoo Vince Community Patch`
if you had an earlier patch, see below).

## What it changes in the game folder

Unlike the earlier VinceFix (which only added a DLL), this patch **does change game files**:

- `Vince.exe` and a number of game data files under `vincedata` (level archives and a few settings files) are replaced
  by patched copies.
- It adds `X3DAudio1_7.dll` (fixes and audio), `d3d11.dll` (water and heat shimmer) and `VinceAIO.ini` (settings).
- It keeps the original of every file it replaced in a `VinceAIO.backup` folder, and writes a record of what it did
  in `VinceAIO.receipt.json`. Leave both alone: uninstall needs them.
- While playing, the game may write log files such as `VinceFix.log` next to `Vince.exe`.

Every file is checked before and after it is written. If a file is not the one the installer expects, it stops and
changes nothing. If the PC crashes or the window is closed halfway, run `Install.cmd` again: it shows
`State: PENDING ...` and you can finish (`I`) or roll back (`U`).

**Undo:** uninstall as above. It restores every original from `VinceAIO.backup` byte for byte, then removes the
backup folder, the record and the two DLLs. Log files are not removed; delete them if you like.

**Steam's "Verify integrity of game files"** does not remove the patch, but it can put some original files back
under it. If you did that, run `Install.cmd`, choose `I` and give the same answers as before: it repairs the
installed set.

## Settings: VinceAIO.ini

`VinceAIO.ini` sits next to `Vince.exe` and is read when the game starts. Open it with Notepad; every line is
explained inside. The main ones:

- `[VinceFix]` `FrameCap=60` (0 = off), `PresentHold=auto`, `HoldFlush=auto`, `LiveReload=0`: frame pacing. If you use
  a frame cap in your graphics driver or RTSS, you can turn it off: the patch's own cap measured best (it steps aside
  by itself when another cap is active).
- `[VinceAudio]` `MasterVolume=0.5`: the overall volume, set to half. Change the value to anything from 0 to 1;
  1.0 is the original game volume.
- `[VinceSteam]` `enabled=0` turns the heat shimmer off.

The installer writes `VinceAIO.ini` only when there is none. Changing options or reinstalling keeps your edits. On
uninstall, a `VinceAIO.ini` you edited stays in place and the installer says so (`Note: VinceAIO.ini: edited since
install; left in place`); delete it yourself if you want it gone. A `VinceAIO.ini` you made yourself before installing
is never touched.

## If you used an earlier patch

The installer recognises these earlier releases and upgrades over them, no need to remove them first:

- **VinceFix 1.3** (`X3DAudio1_7.dll`, `VinceFix.ini`)
- **VinceAudio 1.1.0 and 1.3.0** (patched `Vince.exe` with its `Vince.exe.vinceaudio-backup`)
- **VinceWaterFix v7.10** and **0.7.10.2-experimental** (`d3d11.dll`)
- `VinceSteam.ini` from **VinceSteam AIO v7**

Their files are backed up under `VinceAIO.backup\prior`, and the settings from `VinceFix.ini`, `VinceAudio.ini` and
`VinceSteam.ini` are carried into the new `VinceAIO.ini` (the old file is moved to the backup). A value that is not
plain letters, digits and dots is not carried over and the installer prints a `Note:` saying which one.
Uninstall puts the earlier release back exactly as it was. `VinceAudio.dll` and `VinceAudio.install.json` from
VinceAudio stay in place; the patched game does not use them.

Anything else is refused, with the file's name, and nothing is changed. For example:

- `Refused: X3DAudio1_7.dll: already present and not installed by Voodoo Vince Community Patch; remove it first`: an older VinceFix
  (1.0 to 1.2) or another mod's DLL. Delete that file (and its `.ini`) and run again.
- `Refused: <file>: sha256 ... is neither stock nor recorded by Voodoo Vince Community Patch (another mod?); nothing was changed`: another
  mod changed that game file. Uninstall that mod, or let Steam *Verify integrity of game files*, then run again.
- `Refused: Vince.exe: not the stock Steam build nor a Voodoo Vince Community Patch build ...`: a different game version or another exe
  patch. Verify the game files in Steam first.

## Other messages you may see

- `Refused: Vince.exe is running (pid ...); close the game first`
- `Refused: no Steam install of Voodoo Vince Remastered found; pass -GameDir <folder>` (only when run with a command,
  see below; double-clicking asks for the folder instead)
- `Bad input: ... does not contain Vince.exe`: the folder you typed is not the game folder.
- `Refused: ...: missing from the game folder (verify the game files in Steam)`
- `Refused: ...: backup is not the stock file (stale backup); nothing was changed`: someone changed the backup folder;
  do not edit `VinceAIO.backup`.
- `Refused: ...: leftover temp file not written by Voodoo Vince Community Patch; remove it first`: delete the named `.tmp` file in the
  game folder and run again.

Every refusal happens before anything is written, unless it says otherwise.

## For command-line users

From a Command Prompt in the `installer` folder:

```
Install.cmd status
Install.cmd install -Options firebug,retexture
Install.cmd install -Options none
Install.cmd change -Options voodoopowerfix
Install.cmd uninstall
```

Add `-GameDir "D:\Games\Voodoo Vince Remastered"` to name the game folder. Exit codes: 0 done, 1 refused, 2 bad input.

## Credits and licence

The patch is MIT-licensed, see [LICENSE](LICENSE). Parts by other people and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the full licence texts are in the `licences` folder. Changes are in
[CHANGELOG.md](CHANGELOG.md).

- Vince retexture (texture and normal map): **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**, with permission.
- Janice cutscene pupil fix: **Chameleon2606**, with permission.
- Ultrawide fix, with the particle shape fix: **Chameleon2606**, with permission.

Not affiliated with the game's developer or publisher.
