# Enginehost WOLF RPG Editor core: the wrapper

This branch, `plugin-core`, is the Enginehost wrapper for the Wolf core and nothing else
(Enginehost `docs/plugin-lines.md`). The engine is our own interpreter and lives on `creation`
(`core/`, `docs/behaviour/`); a line, `plugin/<id>`, is `creation` at the commit
`enginehost/lines/<id>/line.json` pins with this branch written over it, and is the only branch
that builds a bundle.

## What the wrapper is

- `WolfPlugin` (`app/src/main/java/dev/enginehost/plugin/wolf/`) runs two ways
  (Droidtop/tracker#858, comment 6691411927):
  - **with a display**, in Enginehost's runtime process under allocate-then-clamp, as the other GL
    cores run: a `GLSurfaceView` in the display, whose GL thread runs the game's 60 fps frames and
    draws them with GLES 2 (`wolf_gl.c`); sound goes out through AAudio (`wolf_aaudio.c`). The
    bundle therefore does not declare `isolatable`. When the Fibrewall GPU proxy's Android build
    lands (#815), GL moves into the isolated process and the bundle declares it again;
  - **isolated, step-driven**, the fallback: the host calls `step()` once a frame and draws the
    pixels the core's software renderer fills; sound goes into the host's audio ring.
- `app/src/main/cpp/`:
  - `wolf_open.c` finds and loads a game's project data with the core's loaders;
  - `wolf_files.c` finds the files commands name (loose first, then archives);
  - `wolf_frame.c` runs the frames and decodes images and sounds;
  - `wolf_enginehost.c` is the JNI side. It's bound by `JNI_OnLoad` in the runtime process, and by
    `enginehost_register_natives` when isolated.

  One library, `libwolfrpg.so`, for arm64-v8a and x86_64, with the core linked in statically.
- Enginehost's `plugin-native/` (the file layer and the audio ring) is not kept here: the
  workflow's plugin-api action checks it out into `build/enginehost/` at the pinned Enginehost
  commit (Droidtop/tracker#921 item 17). Isolated, every libc file call goes through the host's
  broker, so the core reads only the game folder it is handed. In the runtime process the same
  calls go straight to libc, under the clamp. The core never writes the game folder
  (`writesGameFolder: false`).

## What it does now (Droidtop/tracker#858)

On start it opens the game folder and loads Game.dat, CommonEvent.dat and the three databases,
from `Data/BasicData` (unpacked) or from `Data.wolf` / `Data/BasicData.wolf` (3.x archives opened
with the library's default key string), and logs what each held under the logcat tag `wolf`. Each
frame runs the self-starting common events and draws the pictures; sound plays. Messages and
choices are headless for now (logged; a choice takes its first entry), and the map is not run yet.

It ends the launch with a plain reason (`host.fail`) for:
- a Pro game (`GamePro.exe`) or protected data (Pro, or the 2.x editor's scrambler);
- an archive that does not open without a game key (a protect key, or a key this core cannot
  derive yet: the key is derived when an archive is opened and never stored);
- a 2.x archive (the reader handles DX archive version 8 only);
- missing or unreadable project data.

## Signing

The repository's signing certificate and public key are not on this branch yet: they come from
the owner with the signing root rotation (Droidtop/tracker#876). Until they and the
`ENGINEHOST_CERTIFICATE_KEY_PEM` secret are in place, a line builds and its `sign` job fails.
