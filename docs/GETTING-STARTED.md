# Getting started — play *Hot Pursuit 2* natively on your PC

🌐 **English** · [Español](GETTING-STARTED.es.md) · [Português (Brasil)](GETTING-STARTED.pt-BR.md) · [Français](GETTING-STARTED.fr.md) · [Deutsch](GETTING-STARTED.de.md) · [Italiano](GETTING-STARTED.it.md)

Welcome! This guide takes you from your PS2 disc to racing on your PC, one step at a time. You'll type a handful of
commands and the rest is done for you. Plan on about **an hour the first time** — mostly waiting while your PC builds
things, so feel free to grab a snack. After that, starting the game is just a double-click.

> [!IMPORTANT]
> You need **your own original disc** of *Need for Speed: Hot Pursuit 2* for PlayStation 2. No game files come with
> this project, and none are downloaded.

> [!WARNING]
> **Saving has not been tested yet.** The game writes to a memory card kept as files in the game folder, but saving and
> loading a career have not been checked from start to finish. Don't count on keeping career progress yet. Single races
> play from start to finish.

---

## The quick way: download

Since version 0.2.0 there's a ready-to-run download, so you can skip installing the tools and building (steps 1, 3
and 4):

1. Download **`HotPursuit2PC-v0.2.0-win64.zip`** from the [latest release](https://github.com/awtocl/HotPursuit2PC/releases/latest).
2. Right-click it → **Extract All…** and pick a folder you like (for example `C:\Games\HotPursuit2PC`).
3. Open the folder and double-click **`HotPursuit2Launcher.exe`**. Windows may say it protected your PC (the programs
   aren't signed): click **More info**, then **Run anyway**.
4. Then follow **step 2** (your disc) and **step 5** (the launcher's setup) below.

You'll need about 4 GB of free space, most of it for the copy of your disc. Prefer to build it yourself, or want the
very latest changes? Follow all the steps below.

## What you need

| You need | Details |
|---|---|
| **The disc** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **North American release**. The code **SLUS-20362** is printed on the disc and on the back of the case. Other regions (for example the European `SLES-50731`) and any edition with a different program are not supported — the installer checks this for you. |
| **A way to read it** | A DVD drive in (or plugged into) your PC. If you already made an `.iso` image of your own disc, that works too. |
| **The PC** | Windows 10 or 11, 64-bit. A graphics card with Direct3D 11 (any card from the last ten years). |
| **Free disk space** | About **15 GB**: 3 GB for the copy of your disc, 6 GB for the C++ build tools (step 1) and 6 GB for the project with its code generator (step 4). |
| **A controller** *(recommended)* | An Xbox controller, or anything Windows sees as one. The keyboard works too. |
| **Internet** | For step 1 and for the first run of the installer (it downloads about 1–2 GB of tools and source code). |

## Step 1 — Install the tools (once)

The game is built on your PC, so it needs three free tools from Microsoft and Git. The easiest way is **winget**,
which comes with Windows 10 and 11.

1. Open the **Start** menu, type `Terminal`, right-click **Terminal** and choose **Run as administrator**.
2. Copy these three lines in, one at a time, pressing Enter after each. Every line takes a few minutes; wait for each
   to finish before starting the next.

   ```powershell
   winget install --id Microsoft.PowerShell -e
   winget install --id Git.Git -e
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

   The third line installs *Build Tools for Visual Studio 2022* with the *Desktop development with C++* workload — the
   C++ compiler the project needs (about 6 GB). A Visual Studio installer window shows its progress.
3. **Restart your PC** when all three are done.

<details>
<summary>Prefer to install them by hand?</summary>

- **PowerShell 7:** from the [Microsoft Store](https://apps.microsoft.com/detail/9mz1snwt0n5d) or Microsoft's
  [PowerShell page](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows).
- **Git for Windows:** from [git-scm.com](https://git-scm.com/download/win); the default options are fine.
- **Build Tools for Visual Studio 2022:** from [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)
  (under *Tools for Visual Studio*). In the installer, tick **Desktop development with C++** and click *Install*.
  If you already have Visual Studio 2022 with that workload, you have nothing to install.

</details>

## Step 2 — Have your disc ready

The launcher copies your disc to the PC in step 5, so the disc does not have to stay in the drive afterwards. Before
step 5, do one of these:

- **Your disc:** put it in the DVD drive.
- **An `.iso` image of your own disc:** double-click the `.iso` file in File Explorer. Windows shows it as a new DVD
  drive (you can eject it after step 5).
- **You already copied the disc's files to a folder:** nothing to do; you can choose that folder in step 5. It must hold
  `SYSTEM.CNF`, `SLUS_203.62` and the `NFSHP2` folder.

## Step 3 — Download HotPursuit2PC (once)

1. Open the **Start** menu, type `PowerShell 7` and open it (not the older "Windows PowerShell").
2. Copy these lines in, pressing Enter after each:

   ```powershell
   cd $HOME\Documents
   git clone https://github.com/awtocl/HotPursuit2PC.git
   cd HotPursuit2PC
   ```

The project is now in `Documents\HotPursuit2PC`. (Please use `git clone` rather than *Download ZIP*: Windows blocks
scripts that come from a downloaded ZIP file, and `git` also makes updating easy.)

## Step 4 — Build the game

In the same PowerShell 7 window, type:

```powershell
pwsh tools/install-native.ps1
```

The script works through five steps and tells you which one it is on:

| Step | What happens | How long |
|---|---|---|
| 1/5 Your disc | Nothing yet: the launcher asks for your disc in step 5 | — |
| 2/5 LLVM | **First time only:** downloads and builds LLVM, the code generator the recompiler uses | **15–40 min** |
| 3/5 Building | Builds HotPursuit2PC: the launcher, the game and the recompiler | 3–10 min |
| 4/5 Preparing | Nothing yet: the launcher does it after checking your disc | — |
| 5/5 Game folder | Puts the three programs into `game-native\` | seconds |

Your PC may get loud and busy during steps 2 and 3; that is normal. When it ends with **"Done …"** in green, the
launcher opens by itself.

## Step 5 — Set up the game in the launcher (once)

The first time, the launcher walks you through four short steps:

1. **Disc** — your DVD drive (or the mounted `.iso`) appears by itself; select **Copy from …** to copy it into the game
   folder (about 3 GB, a few minutes). Or choose **a folder with the disc's files**. If the disc is another region or
   edition, the launcher says so here.
2. **Check** — every file is compared with the original disc.
3. **Prepare** — the game's program is translated into code for your processor (about a minute).
4. **Ready** — the picture quality is already set for your screen. Select **PLAY NOW**.

![The launcher's home screen](assets/launcher-home.png)

## Step 6 — Play

From now on, open **`HotPursuit2Launcher.exe`** in `Documents\HotPursuit2PC\game-native` and select **PLAY**. For a
desktop shortcut: launcher → **Disc & setup** → **Desktop shortcut**.

The launcher works with the mouse, the keyboard (arrows, Enter, Esc, Page Up/Down) or an Xbox controller (A select,
B back, LB/RB pages, Start plays). Its pages:

| Page | What it's for |
|---|---|
| **Play** | Start the game; whether the disc and the game are in order; your play time |
| **Settings** | The picture and display settings, each explained, with the recommendation for your screen |
| **Controls** | Classic or Modern controller layout, remap any key or button, restore the defaults |
| **Memory card** | What the game has saved; back it up and restore a backup |
| **Screenshots** | The pictures you took with **F12** in the game |
| **Disc & setup** | Check all disc files, prepare the game again, use another disc copy, make a desktop shortcut |
| **About** | What this project is, credits, links to this guide and to the issue tracker |

The game starts in full screen with the intro movies; press **Enter** (or Start on the controller) to skip them. The
launcher hides while you play and comes back when you quit.

### Recommended settings

Change them on the launcher's **Settings** page, or while you play with **F1** (or **Select + Start** on the controller).
The setup already chose the **Picture quality** (the internal resolution) for your screen; it decides how sharp the
picture is:

| Your screen | Picture quality |
|---|---|
| 1920 × 1080 (Full HD) | **Quality (4x)** |
| 2560 × 1440 | **Ultra (6x)** |
| 3840 × 2160 (4K) | **Maximum (8x)** |

Higher is smoother and sharper (it renders more pixels than your screen has and averages them). If the game ever
stutters, choose the next lower value.

| Setting | Recommended | Why |
|---|---|---|
| Renderer | **GPU** | The software renderer is slower and kept for comparison |
| Frame rate | **60** | Twice the PS2's 30 fps in races; the game keeps its normal speed |
| Display mode | **Full screen** | Alt+Enter or F11 switches at any time |
| V-Sync | **On** | Steadier motion, no tearing |
| Scaling filter | **Sharp bilinear** | Crisp without jagged edges |
| Aspect ratio | **16:9** | Wider view in races; the menus keep their shape (changing between 16:9 and 4:3 needs a restart) |
| Sharpening | **Off** or **Low** | Higher values can make edges shimmer |
| Scanlines | **Off** | Unless you like the CRT look |
| Anti-aliasing | **FXAA** | Smooths jagged edges; at the highest picture qualities the difference is small |

Settings are saved automatically in `game-native\hp2native.ini`.

### Controls

Pick a layout on the launcher's **Controls** page (or in the game with **F1** → *Controller layout*):

| Action | Keyboard | Controller: Classic (PS2) | Controller: Modern |
|---|---|---|---|
| Accelerate | Z | A | RT (or A) |
| Brake / Reverse | A | X | LT |
| Steer | ← → | Left stick / D-pad | Left stick / D-pad |
| Handbrake | W | RB | B |
| Horn / Siren | X | B | Left stick click |
| Change view | S | Y | Y |
| Look back | Q | LB | Right stick click |
| Speed boost | E | Right stick click | X |
| Shift up / Zone Zoom | 2 | RT | RB |
| Shift down / Zone Freeze | 1 | LT | LB |
| Pause | Enter, Space | Start | Start |
| Cycle HUD | Backspace, Right Shift | Back | Back |
| Settings menu | F1 | Select + Start | Select + Start |
| Screenshot | F12 | – | – |

- **Classic** is the PS2 controller, button for button. **Modern** puts throttle and brake on the triggers, like
  today's racing games.
- **Make your own:** on the **Controls** page, select a box and press the key or button you want. Every action takes
  two keys and two controller buttons. Esc cancels, Delete clears a key. Your first change starts a *Custom* layout
  from the one you were using.
- **Changed your mind?** **Restore Default Keyboard** and **Restore Default Controller** go straight back to the
  presets. Your custom layout stays saved; pick *Custom* to use it again.
- The analog sticks work as the DualShock 2's sticks. PlayStation controllers are not seen by Windows as game
  controllers on their own; they work through a tool that presents them as an Xbox controller, such as Steam's
  controller support.

### Language

The launcher and the in-game settings menu come in English, Spanish, Portuguese (Brazil), French, German and Italian.
The first time, they follow your Windows display language; change it in the launcher under **Settings → Language** or
in the game with **F1**. The game's own text stays in English: that's how the North American disc ships.

### Quitting

**F1 → Quit game** (press it twice to confirm), or **Alt+F4**.

## Updating

When the project gets new features, update and rebuild — your settings, saves and disc copy stay:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1
```

LLVM is not rebuilt, so this takes only a few minutes. The launcher then shows **PREPARE & PLAY**: the game is prepared
again for the new version (about a minute) and starts.

## If something goes wrong

| What you see | What to do |
|---|---|
| *"The C++ build tools were not found"* | Step 1 is not complete: install the Build Tools with **Desktop development with C++**, restart the PC, try again. |
| *"pwsh is not recognized"* | PowerShell 7 is not installed, or the PC was not restarted after step 1. |
| The launcher doesn't find your disc | Wait a few seconds after inserting it; for an `.iso`, double-click it first. Or copy the disc's files into a folder yourself and choose **a folder with the disc's files**. |
| *"This folder does not hold the contents of a PlayStation 2 disc"* or *"… is missing"* | You chose the wrong folder, or the copy is incomplete. Choose the folder that directly holds `SYSTEM.CNF`, `SLUS_203.62` and `NFSHP2`. |
| *"This disc is SLES-…"* or *"… is not the supported version"* | Your disc is another region or edition. Only the North American `SLUS-20362` release is supported. |
| *"… differs from the original disc"* | The disc copy is damaged (often a scratched disc). Clean the disc and copy it again (**Disc & setup** → **Change disc**). |
| *"Preparing the game failed"* | The details are in `game-native\prepare.log`; please attach it to an [issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"The game stopped: …"* on the launcher's Play page | Select **Open log** and **Report a problem**; attach `native.log`. |
| *"Downloading LLVM failed"* | Check your internet connection and run the installer again. |
| *"The build failed"* | Copy the red lines and [open an issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"…cannot be loaded because running scripts is disabled"* | You downloaded a ZIP instead of using `git clone`. Use `git clone` (step 3). |
| The game window is black, or the picture is wrong | Launcher → **Settings** → Renderer → **Software**. If that helps, please report it with your graphics card model. |
| No sound | The game uses Windows' default playback device: check it in the Windows sound settings, then restart the game. |
| The controller does nothing | Only Xbox-compatible (XInput) controllers are seen; see [Controls](#controls). The keyboard always works. |
| A button does the wrong thing | Launcher → **Controls** → **Restore Default Controller** (or **Restore Default Keyboard**). |
| It stutters | Lower the **Picture quality** (launcher → Settings) and make sure V-Sync is On. |
| Everything looks wrong after changing settings | Launcher → **Settings** → **Reset to defaults**. |

**Reporting a problem:** after the game closes, the file `game-native\native.log` describes what happened. Attach it
to an [issue](https://github.com/awtocl/HotPursuit2PC/issues) with your Windows version and graphics card. The log
contains no personal data apart from folder paths, which you can remove.

## Uninstalling

Nothing is installed into Windows by this project itself. To remove it, delete the `Documents\HotPursuit2PC` folder
(the build tools live inside it, in `.cache`, and the copy of your disc in `game-native\disc`). Back up
`game-native\saves` first if you want to keep your saves. A desktop shortcut made by the launcher can be deleted like
any other. The tools from step 1 can be removed in *Settings → Apps* like any other program.

---

Stuck, or something in here unclear? [Open an issue](https://github.com/awtocl/HotPursuit2PC/issues) and let us
know — this guide should work for everyone. Have fun out there, and watch out for the cops!
