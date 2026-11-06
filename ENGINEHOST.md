# Enginehost WOLF RPG Editor core: the wrapper

This branch, `plugin-core`, is the Enginehost wrapper for the Wolf core and nothing else
(Enginehost `docs/plugin-lines.md`). The engine is our own interpreter and lives on `creation`
(`core/`, `docs/behaviour/`); a line, `plugin/<id>`, is `creation` at the commit
`enginehost/lines/<id>/line.json` pins with this branch written over it, and is the only branch
that builds a bundle.

## What the wrapper is

- `WolfPlugin` (`app/src/main/java/dev/enginehost/plugin/wolf/`): a step-driven `EnginePlugin`
  that runs only in Enginehost's isolated runtime (`isolatable`, Enginehost
  `docs/engine-sandbox.md`). The host calls `step()` once a frame and draws the pixels it fills.
- `app/src/main/cpp/`: `wolf_open.c` finds and loads a game's project data with the core's
  loaders; `wolf_enginehost.c` is the JNI side, bound by `enginehost_register_natives`. One
  library, `libwolfrpg.so`, for arm64-v8a and x86_64, with the core linked in statically.
- Enginehost's `plugin-native/` (the file layer and the audio ring) is not kept here: the
  workflow's plugin-api action checks it out into `build/enginehost/` at the pinned Enginehost
  commit (Droidtop/tracker#921 item 17). Every libc file call in the library goes through the
  host's broker, so the core reads only the game folder it is handed. The bundle does not write the game folder yet (`writesGameFolder: false`).

## What it does now (first milestone, Droidtop/tracker#858)

On start it opens the game folder and loads Game.dat, CommonEvent.dat and the three databases,
from `Data/BasicData` (unpacked) or from `Data.wolf` / `Data/BasicData.wolf` (3.x archives opened
with the library's default key string), and logs what each held under the logcat tag `wolf`.
Each step is a black frame of the game's screen size; the interpreter does not run yet.

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
