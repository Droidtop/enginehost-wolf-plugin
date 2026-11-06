# Saves

What a Wolf save holds and when it is written, from the editor's manual. Then what the core
does, given that the original runtime's save files are encrypted.

## The original runtime's save files: not readable without breaking their encryption

- The files are `Save/SaveDataNN.sav` for save number NN (`SaveData01.sav` for 1). A save
  number may be a call value [V, manual 04ev_file.html].
- A save number that is a string variable (3,000,000 up) names the file itself, relative to the
  game folder: "システム情報.sav" lands beside Game.exe, "Save/システム.sav" in `Save/`. Since
  3.00 the runtime refuses a name holding `//`, `\\`, `..`, `./`, `.\`, `%`, `:`, `*`, `?`, `"`,
  `<`, `>` or `|` [V, 04ev_file]. The owner's Living With Sister has such a file, `System.sav`,
  next to its numbered saves.
- **The runtime encrypts every save.** The manual names "the encryption used when saving"
  (セーブデータ保存時の暗号化) and says it switches to ChaCha20 when a game is packed with the
  ChaCha20 option (3.50 on) [V, manual 02gamemake.html]. The owner's games agree: the three
  `.sav` files on G: (Living With Sister 3.191, WitchLegacy 3.659) have no readable structure
  past their first bytes. Only those first bytes were looked at, in place.
- **So compatibility with the original runtime's saves is Unknown, and the core does not try.**
  Reading them would mean recovering the save key, which the lane's rules forbid
  (Droidtop/tracker#858: no key derivation, no brute force). Neither the manual nor the open
  tools (UberWolf, wolftrans, WolfTL, popupie/wolftools, mruby-wolf) document the save layout.
  mruby-wolf writes its own format and says so.

## What a save holds [V, manual]

What the core's own saves have to cover, so a game behaves the same after a load:
- the normal, spare, string and system variables;
- the changeable database: its values, **and its type and data layout as it was when this play
  started** (01FAQ.html item 10: a save made before the editor added fields keeps the old field
  count, and fields added at the end are read as new). The user and system databases are not
  saved; they always come from the game's files;
- XY arrays (04ev_file: big XY arrays make big saves);
- the map, the hero's place, and the map events' state (self variables and positions). The map's
  tiles are saved too, unless system variable 127 is 1, in which case a load reads the tiles
  back from the `.mps` and tile changes made by events are lost; events are saved either way
  [V, 06systemvalue.html];
- an identity of the game: changing the game's main title makes old saves unusable, while the
  title's suffix (the version text) can change freely [V, 02gamesetting.html]. The local lane's
  earlier note also mentions a stored random seed and a "different game" check [V, local lane];
  how the core matches the identity is below.

## When saving and loading happen [V, manual 04ev_file.html]

- **A load restarts with no event running**, even if the save was made in the middle of one.
  A thread's place in its command list is not saved, only the state above.
- A load from a parallel event is an error that stops the event.
- Loading a save that does not exist does nothing, and the next command runs. Loading one that
  exists stops the current event at once and resumes the saved play.
- **221 / 222** (read one variable or string from a save, write one to a save):
  - they touch one value in a save file without loading it;
  - reading from a missing save gives 0 or "";
  - writing to a missing save creates it, holding only variables, with no changeable database;
  - map event self variables and XY arrays cannot be read this way.
- Writes from 222 reach the file at the end of the frame. Saves touched by 221/222 are cached in
  memory, and the cache is dropped when it would overflow.
- System variable 24 is 1 when the save being read through 221 exists, which is how games test
  for a save. System variable 25 is the save number this play was saved to or loaded from, or 0
  for a save named by a string [V, 06systemvalue.html].

## What the core does

- **Its own format, never the original runtime's.** The core writes and reads only files it made
  itself. A `.sav` from the Windows runtime is not opened. A game started under Enginehost
  starts from a new game, and the game page says that saves from the Windows version don't carry
  over. Saves from the core can't be loaded by the Windows runtime either.
- **Where.** The game folder is read-only to the core (`writesGameFolder: false`). Saves go to the
  per-game save directory Enginehost hands the core, under the same names the runtime would use
  (`SaveData01.sav`, or the string-named file). A string name holding any of the manual's
  refused sequences is refused the same way, and so is any absolute path (risks.md).
- **What goes in:** everything in "What a save holds", with every count and length bounded on
  the way back in, like every other reader in the core. The layout is decided when the save
  code is written, and gets its own section here first.
- **Identity:** the game's main title (Game.dat text 0) is stored, and a save whose title differs
  is refused with the runtime's "save data from a different game" meaning [M]. Whether the
  runtime also compares anything else is **Unknown**.
- **Encryption:** none. The original runtime encrypts to stop players editing saves. The core's
  saves stay in the player's own save directory, so there's nothing to protect them from.

Not built yet: 220 (save and load), 221 and 222.
