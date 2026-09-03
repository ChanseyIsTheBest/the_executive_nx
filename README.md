# The Executive — Nintendo Switch port (GLSurfaceView / JNI wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
The Executive on Switch homebrew. It contains no game code and no game assets —
it loads the game's own libraries and recreates, natively, the thin Android/JNI
layer the engine expects. It is a fork of the Pizza Vs. Skeletons wrapper: same
studio, same engine, and the two import sets differ by eleven entries out of 238.
 
## Install & run
 
You need files from The Executive V.1.1.0.
 
```
sdmc:/switch/executive
├── executive_nx.nro
├── libexecutive_android.so  libc++_shared.so   <- from your APK: lib/arm64-v8a/
├── cursor.png                                  <- optional
├── assets/                                     <- the base APK's assets/, whole
└── files/                                      <- created on first launch; saves live here
```
 
 Launch via title override (hold R while starting an installed game) or a forwarder.
 
Optionally drop a `cursor.png` (up to 64×64, transparency respected, top-left
pixel is the hotspot) in the same folder to replace the on-screen cursor.
 
## Controls
 
The Executive is a touch game — tap to strike, hold to block, swipe for stunts
and Strong Kicks — so in handheld the touchscreen is the whole control scheme
and everything below is for playing it on a pad.
 
| Input | Action |
|---|---|
| Touchscreen | Direct multi-touch (handheld) |
| Left stick | Move the cursor |
| **A** / **ZL** / **ZR** | Tap at the cursor; **hold to block** (ZL and ZR let you play one-handed) |
| Right stick | **Flick to swipe** — a press-drag-release from the cursor, along the direction you flicked |
| **B** / **+** | Back |
| **Y** | Space |
| **−** | Toggle the on-screen cursor |
| **R3** | Toggle gyro pointing (tilt/turn the controller to aim) |
| **L** / **R** | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| **L3** | Recalibrate the accelerometer (does nothing in the default mode) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
 
A USB mouse works in both modes: move to control the cursor, left-click to tap, scroll wheel to
change sensitivity — gyro turns itself off while a mouse is connected. Your
stick, mouse and gyro sensitivities are remembered in `pointer.cfg`
automatically after in-game adjustment.
 
## Settings
 
`config.txt` is written next to the `.nro` on first launch, with the options
documented inline. The ones worth knowing:
 
```
cursor_stick     0      # which stick moves the cursor: 0 left, 1 right
swipe_mode       0      # 0 flick, 1 drag
swipe_stick      1      # which stick flicks: 0 left, 1 right
swipe_len        420    # swipe length in pixels at 1080p
swipe_frames     5      # frames to spread the swipe over
accel_mode       0      # 0 static, 1 driven by the console gyro
language         auto   # auto, or one of: en fr de it es
```
 
**If stunts register as taps, raise `swipe_frames` first** — it is the one
number in the port chosen by reasoning rather than measurement.
 
## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib \
          switch-libwebp switch-ffmpeg switch-pkg-config
 
export DEVKITPRO=/opt/devkitpro
make                        # -> executive_nx.nro
```
 
| Command | Result |
|---|---|
| `make` | Release. A small `executive_nx.log` still records INFO and above. |
| `make DEBUG=1` | Bring-up. Everything, including the per-file `asset miss` flood — loading is visibly slower. |
| `make LOG=0` | Silent. No log file is created. |
| `make MUSIC=0` | Compiles the music path out; sound effects are unaffected. |
 
## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `imports_helpers`,
`nx_pointer`, `compat_stubs`) derives from the open-source Switch `.so`-loader
lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on TheOfficialFloW's
Vita/Switch loader tradition — reaching this project via the Killer Bean
Unleashed, Sonic Jump and Osmos ports, and then via the Pizza Vs. Skeletons
port, which is where almost all of this one came from. All MIT-licensed. Thanks
to everyone in that lineage for making this approach possible.
