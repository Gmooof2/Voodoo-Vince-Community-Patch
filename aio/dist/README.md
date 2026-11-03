<img width="1920" height="1080" alt="674037262-bb2d0131-daf7-401c-ad75-d86fe0b4eaf9" src="https://github.com/user-attachments/assets/5168e260-1958-451e-9249-8d5a9427a0ed" />

One patch for **Voodoo Vince Remastered** with the community fixes: graphics, sound, frame pacing, crashes and a few
look-and-feel touches. Its installer lets you pick three optional parts, change them later, and put the game back
exactly as it was.

It needs the **Steam** version, **1.14.2.0** (the current one), on Windows 10 or 11. Any other version is refused
before anything is written.

## What it fixes

- **Performance:** the game runs at your real resolution when Windows scaling is above 100 %, its threads are no
  longer pinned to the first CPU cores, and a 60 fps cap gives steadier frame pacing.
- **Sound:** stereo fix; sound survives an audio device change; the power icon pickup sound plays again; the volume
  is set to half (adjustable, see Settings); the loud Kosmobot fan and the Land o' Rides cat noise are turned down.
- **Crashes and cutscenes:** no crash when skipping a cutscene; Charmaine's voice and subtitle; Vince's eye in the
  Janice cutscene (pupil fix by **Chameleon2606**); Vince walks out of warps again.
- **Graphics:** water and the Main Tunnel puddle restored; the Crypt City lights glow and the urns reflect; heat
  shimmer over steam and fire; Kosmo's cabin smoke and the Crypt City gate fog; the bog lightmaps; the bumper car fog
  and obstacles; the Bone Goliath lightning; the butler ghost's field of view; the Zombie Guidance Counselor's ball
  toned down; less flicker on surfaces that sit very close together, like Vince's pupils.
- **Ultrawide:** 21:9 and 32:9 screens fill without black bars (the HUD and menus stay 16:9 in the middle), and
  particles are no longer stretched. By **Chameleon2606**. If you change the resolution or display mode while
  playing, go back to the main menu to put the picture's shape right.

Optional, asked at install:

| Option | What it does |
| --- | --- |
| `voodoopowerfix` | New materials for the power icon, the power skull and the beads. |
| `firebug` | New material for the firebug's shell. |
| `retexture` | Updated Vince texture and normal map, by **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**. |

## Install

1. **Close the game.**
2. **Unzip** `VoodooVinceCommunityPatch-2.0.0.zip` into a folder of its own, not the game folder. **Keep this
   folder:** you need it to change options or uninstall.
3. In its `installer` folder, **double-click `Install.cmd`**. It finds your Steam game by itself. If it asks for the
   folder instead, in Steam right-click the game, then *Manage*, *Browse local files*, and paste that folder.
4. Type `I`, press Enter, and answer `y` or `n` for each option.
5. Keep the window open until it says `Installed set ...`, then press any key and play.

If Windows shows **"Windows protected your PC"**, click **More info**, then **Run anyway**. If there is no such button
(Smart App Control), right-click the zip, choose *Properties*, tick **Unblock**, click OK and unzip again. The
installer is a plain-text PowerShell script you can read: it needs no administrator rights, does not go online, does
not touch the registry and writes only inside the game folder.

## Change options, check, uninstall

Run `Install.cmd` again. Its first lines show what is installed. Type `C` to change options (only the files that
differ are rewritten), `U` to uninstall (every original file comes back byte for byte), or `Q` to leave.

## What it changes

It replaces `Vince.exe` and some game data files, and adds `X3DAudio1_7.dll`, `d3d11.dll` and `VinceAIO.ini`. The
originals are kept in `VinceAIO.backup`, with a record in `VinceAIO.receipt.json`: leave both alone, uninstall needs
them. Every file is checked before and after it is written. If the installer is cut off halfway, run it again to
finish (`I`) or roll back (`U`). After Steam's *Verify integrity of game files*, run `I` again with the same answers to
repair the install.

## Settings

`VinceAIO.ini`, next to `Vince.exe`, is read when the game starts; every line is explained inside. The main ones:

- `[VinceFix]` `FrameCap=60` (0 = off): the frame cap. It measured better than a driver or RTSS cap, and steps aside
  by itself while one is on.
- `[VinceAudio]` `MasterVolume=0.5`: the overall volume, from 0 to 1; 1.0 is the original game volume.
- `[VinceSteam]` `enabled=0`: turns the heat shimmer off.

Your edits are kept when you change options or reinstall, and an edited `VinceAIO.ini` stays in place after uninstall.

## Earlier patches

It upgrades over **VinceFix 1.3**, **VinceAudio 1.1.0 and 1.3.0**, **VinceWaterFix v7.10 and 0.7.10.2-experimental**
and the `VinceSteam.ini` from VinceSteam AIO v7, with no need to remove them first. Their settings carry over into
`VinceAIO.ini`, and uninstall puts the earlier release back.

## If it refuses

A refusal names the file and changes nothing. The usual causes:

- **The game is running:** close it.
- **An older VinceFix (1.0 to 1.2) or another mod's file:** delete the named file (and its `.ini`), or uninstall that
  mod, then run again.
- **A different game version, or a game file another mod changed:** let Steam *Verify integrity of game files*, then
  run again.

## Command line

From the `installer` folder: `Install.cmd status`, `Install.cmd install -Options firebug,retexture` (or `none`),
`Install.cmd change -Options voodoopowerfix` and `Install.cmd uninstall`. Add `-GameDir "<game folder>"` to name the
game folder. Exit codes: 0 done, 1 refused, 2 bad input.

## Credits and licence

The patch is MIT-licensed, see [LICENSE](LICENSE). Parts by other people and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the full licence texts are in the `licences` folder. Changes are in
[CHANGELOG.md](CHANGELOG.md).

- Vince retexture, Janice cutscene pupil fix, and the ultrawide fix with its particle shape fix:
  **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**, all included with permission.

Not affiliated with the game's developer or publisher.
