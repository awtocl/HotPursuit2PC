# Getting started — play *Hot Pursuit 2* natively on your PC

This guide takes you from your PS2 disc to playing, step by step. You type a handful of commands, and one script does
the rest. Expect about **an hour the first time**, mostly waiting while your PC builds things; after that, starting the
game is a double-click.

> [!IMPORTANT]
> You need **your own original disc** of *Need for Speed: Hot Pursuit 2* for PlayStation 2. No game files come with
> this project, and none are downloaded.

> [!WARNING]
> **Saving is not implemented yet.** The game runs as if no memory card were inserted, so career progress is lost when
> you quit. Single races play from start to finish.

---

## What you need

| | |
|---|---|
| **The disc** | *Need for Speed: Hot Pursuit 2*, PlayStation 2, **North American release**. The code **SLUS-20362** is printed on the disc and on the back of the case. Other regions (European `SLES-…`, Japanese `SLPM-…`) and "Greatest Hits" reprints with a different program are not supported — the installer checks this for you. |
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

## Step 2 — Copy your disc to the PC (once)

The game reads its data from a copy of your disc, so the disc does not have to stay in the drive.

1. Make a folder for it, for example `C:\Games\HP2 disc`.
2. Put the disc in the DVD drive and open the drive in **File Explorer** (*This PC* → the DVD drive).
3. Select everything on it (**Ctrl+A**) and copy it (**Ctrl+C**) into your new folder (**Ctrl+V**). It is about
   **3 GB** and takes a few minutes.

When it is done, the folder must hold at least these three items:

```
C:\Games\HP2 disc\
    NFSHP2\          (a folder)
    SLUS_203.62
    SYSTEM.CNF
```

**If you have an `.iso` of your own disc instead:** double-click the `.iso` file. Windows shows it as a new DVD drive;
copy its contents exactly as above, then eject it.

> [!TIP]
> Keep this folder where it is: the game reads it every time it runs (it never changes anything in it). If you move
> it, run the installer again (step 4).

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

A window asks for your disc folder: choose the folder from step 2 (if you don't see the window, it may be hidden
behind the PowerShell one). Then the script works through five steps and tells you which one it is on:

| Step | What happens | How long |
|---|---|---|
| 1/5 Your disc | Checks the folder and that your disc is the supported version | seconds |
| 2/5 LLVM | **First time only:** downloads and builds LLVM, the code generator the recompiler uses | **15–40 min** |
| 3/5 Building | Builds HotPursuit2PC | 3–10 min |
| 4/5 Recompiling | Translates the game's program from your disc into native PC code | a few minutes |
| 5/5 Game folder | Puts everything into `game-native\` | seconds |

Your PC may get loud and busy during steps 2 and 3; that is normal. When it ends with **"Done … To play,
double-click"** in green, you are ready.

## Step 5 — Play

Open `Documents\HotPursuit2PC\game-native` and double-click **`Play.cmd`**.

To make a desktop shortcut: right-click `Play.cmd` → *Show more options* → **Send to → Desktop (create shortcut)**.

The game starts in full screen with the intro movies; press **Enter** (or Start on the controller) to skip them.

### Recommended settings

Press **F1** in the game (or **Select + Start** on the controller) to open the settings. The defaults are already
good; the one worth adjusting is **Internal resolution**, which decides how sharp the picture is:

| Your screen | Internal resolution |
|---|---|
| 1920 × 1080 (Full HD) | **4x** (default) |
| 2560 × 1440 | **6x** |
| 3840 × 2160 (4K) | **8x** |

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

Settings are saved automatically in `game-native\hp2native.ini`.

### Controls

**An Xbox controller** works exactly like a PS2 DualShock 2 (A = ✕, B = ○, X = □, Y = △, Start, Back = Select, the
bumpers and triggers = L1/R1/L2/R2). PlayStation controllers are not seen by Windows as game controllers on their own;
they work through a tool that presents them as an Xbox controller, such as Steam's controller support.

**On the keyboard:**

| Key | PS2 button | Key | PS2 button |
|---|---|---|---|
| Arrows | D-pad / steering | Enter, Space | Start |
| Z | ✕ (accelerate) | Backspace, Right Shift | Select |
| X | ○ | Q / W | L1 / R1 |
| A | □ | 1 / 2 | L2 / R2 |
| S | △ | F1 | Settings |

The analog sticks work as the DualShock 2's sticks. Steering with the left stick or the arrows, ✕ accelerates.

### Quitting

**F1 → Quit game** (press it twice to confirm), or **Alt+F4**.

## Updating

When the project gets new features, update and rebuild — your settings stay:

```powershell
cd $HOME\Documents\HotPursuit2PC
git pull
pwsh tools/install-native.ps1 -Rom "C:\Games\HP2 disc"
```

(Use your own disc folder. LLVM is not rebuilt, so this takes only a few minutes.)

## If something goes wrong

| What you see | What to do |
|---|---|
| *"The C++ build tools were not found"* | Step 1 is not complete: install the Build Tools with **Desktop development with C++**, restart the PC, try again. |
| *"pwsh is not recognized"* | PowerShell 7 is not installed, or the PC was not restarted after step 1. |
| *"… does not look like the contents of the disc: … is missing"* | You chose the wrong folder, or the copy is incomplete. Choose the folder that directly holds `SYSTEM.CNF`, `SLUS_203.62` and `NFSHP2`. |
| *"SLUS_203.62 is not the supported version"* | Your disc is another region or edition. Only the North American `SLUS-20362` release is supported. |
| *"Downloading LLVM failed"* | Check your internet connection and run the installer again. |
| *"The build failed"* | Copy the red lines and [open an issue](https://github.com/awtocl/HotPursuit2PC/issues). |
| *"…cannot be loaded because running scripts is disabled"* | You downloaded a ZIP instead of using `git clone`. Use `git clone` (step 3). |
| The game window is black, or the picture is wrong | F1 → Renderer → **Software**. If that helps, please report it with your graphics card model. |
| No sound | The game uses Windows' default playback device: check it in the Windows sound settings, then restart the game. |
| The controller does nothing | Only Xbox-compatible (XInput) controllers are seen; see [Controls](#controls). The keyboard always works. |
| It stutters | Lower the Internal resolution (F1) and make sure V-Sync is On. |
| Everything looks wrong after changing settings | Close the game and delete `game-native\hp2native.ini`; the defaults come back. |

**Reporting a problem:** after the game closes, the file `game-native\native.log` describes what happened. Attach it
to an [issue](https://github.com/awtocl/HotPursuit2PC/issues) with your Windows version and graphics card. The log
contains no personal data apart from folder paths, which you can remove.

## Uninstalling

Nothing is installed into Windows by this project itself. To remove it, delete the `Documents\HotPursuit2PC` folder
(the build tools live inside it, in `.cache`). Your disc folder from step 2 is yours to keep or delete. The tools from
step 1 can be removed in *Settings → Apps* like any other program.

---

Something unclear or missing in this guide? [Open an issue](https://github.com/awtocl/HotPursuit2PC/issues) — this
guide should work for everyone.
