<img width="1920" height="1080" alt="674037262-bb2d0131-daf7-401c-ad75-d86fe0b4eaf9" src="https://github.com/user-attachments/assets/5168e260-1958-451e-9249-8d5a9427a0ed" />

# Voodoo Vince Remastered: Community Patch

A patch for **Voodoo Vince Remastered** that brings together community fixes for graphics, sound, frame pacing,
crashes and a few look-and-feel touches. Pick optional extras at install, change them later, or remove the patch and
get the original game back exactly as it was.

> **Requires:** the **Steam** version **1.14.2.0** (the current one) on Windows 10 or 11. Any other version is refused
> before anything is written.

---

## ✨ What it fixes

### 🎮 Performance
- Runs at your real resolution when Windows scaling is above 100 %.
- Threads are no longer pinned to the first CPU cores causing stuttering when background application's are running (Discord and OBS a particular example).
- A 60 fps cap with an enhanced frame limiter gives steadier frame pacing (some physics breaks above 60 like the boat race obstacles).

### 🔊 Sound
- Stereo fix, and audio device caching fixing stutters.
- Sound survives an audio device change and no longer cuts out.
- The power icon sound plays again (percentage chance of it not working).
- Volume is set to half by default (adjustable in Settings).
- The loud Kosmobot fan and the Land o' Rides cat noises are turned down.

### 🎬 Crashes and cutscenes
- No crash when skipping the end of the Janice bossfight cutscene.
- Charmaine's cut third line in Main Tunnel.
- Vince's eye animation in the first Janice cutscene (pupil fix by **Chameleon2606**).
- Bulldozer cutscene fix.
- Vince walks out of warps again.

### 🎨 Graphics
- Updated Vince texture and normal map to resemble the Xbox Original Vince.
- Water shader and visuals restored, plus the puddle in the main tunnel.
- Crypt City lights glow, and the urns reflect properly.
- Heat shimmer over steam and fire, including the propeller room.
- Smoke in Kosmo's cabin after hitting the barriers, and the fog at the Crypt City gate.
- Lightmap fix in the bog.
- Bumper car fog increased, with obstacle animations from the prototype restored.
- Bone Goliath (dino boss) lightning flashing UI icons at higher resolutions.
- Butler ghost field of view fixed; the Zombie Guidance Counselor's ball look from the Xbox.
- Camera near plane moved from 0.1 to 0.05, which reduces flickering on surfaces that sit very close together, like
  Vince's pupils.
- Camera FOV fix in the harp puzzle.
- Particle texture stretch fixed.

### 🖥️ Ultrawide
- 21:9 and 32:9 screens fill without black bars. The HUD and menus stay 16:9 in the middle.
- By **Chameleon2606**.
- If you change resolution or display mode during play, return to the main menu to fix the picture's shape.

---

## 📦 Install

1. **Close the game.**
2. **Unzip** `VoodooVinceCommunityPatch-2.0.0.zip` into its own folder, not the game folder. **Keep this folder**, as
   you need it to change options or uninstall.
3. Open the `installer` folder and **double-click `Install.cmd`**. It finds your Steam game automatically. If it asks
   for the folder, right-click the game in Steam, choose **Manage → Browse local files**, and paste that path.
4. Type `I` and press Enter, then answer `y` or `n` for each optional part.
5. Wait for `Installed set ...`, press any key, and play.

### Optional parts

| Option | What it adds |
| --- | --- |
| `voodoopowerfix` | New materials for the power icon, power skull, beads and firebug. |
| `firebug` | New material for the firebug's shell. |
| `retexture` | Updated Vince texture and normal map by **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**. |

### ⚠️ If Windows blocks the installer

- **"Windows protected your PC":** click **More info**, then **Run anyway**.
- **No such button (Smart App Control):** right-click the zip, open *Properties*, tick **Unblock**, click OK, and unzip again.

The installer is a plain-text PowerShell script you can read. It needs no administrator rights, never goes online,
doesn't touch the registry, and only writes inside the game folder.

---

## 🔁 Change, check or uninstall

Run `Install.cmd` again. Its first lines show what is installed, then:

- `C`: change options (only files that differ are rewritten)
- `U`: uninstall (every original file comes back byte for byte)
- `Q`: quit

---

## ⚙️ Settings

`VinceAIO.ini`, next to `Vince.exe`, is read when the game starts. Every line is explained inside. The main settings:

| Setting | Default | Effect |
| --- | --- | --- |
| `[VinceFix] FrameCap` | `60` | Frame cap. `0` turns it off. It steps aside on its own while a driver or RTSS cap is active. |
| `[VinceAudio] MasterVolume` | `0.5` | Overall volume, from `0` to `1`. `1.0` is the original game volume. |
| `[VinceSteam] enabled` | on | Heat shimmer. Set to `0` to turn it off. |

Your edits are kept when you change options or reinstall. An edited `VinceAIO.ini` stays in place after uninstall.

---

## 🧩 What it changes

The patch replaces `Vince.exe` and some game data files, and adds `X3DAudio1_7.dll`, `d3d11.dll` and `VinceAIO.ini`.

The originals are saved in `VinceAIO.backup`, with a record in `VinceAIO.receipt.json`. **Leave both alone**, because
uninstall needs them. Every file is checked before and after it is written.

- If the installer is interrupted, run it again to finish (`I`) or roll back (`U`).
- After Steam's *Verify integrity of game files*, run `I` again with the same answers to repair the install.

### Upgrading from earlier patches

It upgrades over **VinceFix 1.3**, **VinceAudio 1.1.0 and 1.3.0**, **VinceWaterFix v7.10 and 0.7.10.2-experimental**,
and the `VinceSteam.ini` from VinceSteam AIO v7. There is no need to remove them first. Their settings carry over into
`VinceAIO.ini`, and uninstall restores the earlier release.

---

## 🛠️ Troubleshooting

If the installer refuses, it names the file and changes nothing. Common causes:

| Problem | Fix |
| --- | --- |
| The game is running | Close it. |
| An older VinceFix (1.0 to 1.2) or another mod's file is present | Delete the named file (and its `.ini`), or uninstall that mod, then run again. |
| Different game version, or a file changed by another mod | In Steam, use *Verify integrity of game files*, then run again. |

---

## 💻 Command line

Run these from the `installer` folder:

```bash
Install.cmd status
Install.cmd install -Options firebug,retexture
Install.cmd change -Options voodoopowerfix
Install.cmd uninstall
```

- Use `-Options none` to install with no optional parts.
- Add `-GameDir "<game folder>"` to name the game folder yourself.
- Exit codes: `0` done, `1` refused, `2` bad input.

---

## 🙏 Credits and licence

- Vince retexture, Janice cutscene pupil fix, and the ultrawide fix with its particle shape fix by
  **[Chameleon2606](https://www.nexusmods.com/voodoovinceremastered/mods/7)**, included with permission.
- The patch is MIT-licensed. See [LICENSE](LICENSE).
- Third-party parts and their licences are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Full licence
  texts are in the `licences` folder.
- Changes are listed in [CHANGELOG.md](CHANGELOG.md).

*Not affiliated with the game's developer or publisher.*
