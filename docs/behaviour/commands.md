# Event commands: the interpreter

Sources are marked as in the other notes: [V] read in a public source or traced by the local lane,
[M] our own reading, to be checked by the comparison runs. [R] is behaviour learned by reading
take-cheeze/rpg-maker-clone's `mruby-wolf` (MIT) at commit 75b1c34, as the owner allowed on
#858 ("we can learn from it"): read only, never built, run, copied or vendored. What is written
here is the behaviour in our own words; the code is our own. **Unknown** marks what no source
settles yet; the core does the safe thing named and logs it.

## The dispatcher (`core/src/vm.c`)

Built to the coordinator's recommendation while the owner was away (2026-11-05); **owner to
confirm the shape**. Each choice below is meant to be easy to change.

- **One handler per command code**, in a table indexed by code (0 to 1000), as the runtime
  switches on the code [V, the local lane's static pass on #858]. A code with no handler is logged
  once and skipped, so a game keeps running while commands are filled in, and the log says which
  are missing [M: recommended over stopping].
- **A thread** is one running event: a stack of command lists, each with its own program counter.
  Calling a common event pushes a list; reaching the end of a list (or the -1 end command) pops it
  [V]. Calls nest at most 256 deep [M: a safety cap, not the runtime's number].
- **A handler answers carry on or yield** [V]. A command that waits (a message, a choice, a wait)
  yields; the thread resumes there on the next frame.
- **5,000,000 commands per frame** at most, then the thread stops with the runtime's "infinite
  loop" error, the same limit so behaviour matches [V].
- **Nothing outside the core's state is touched directly.** Logging (and later drawing, sound,
  input, files) goes through a host interface the plugin provides, so the sandbox rules hold
  (docs/behaviour/risks.md).

## Command lists (`core/src/wolf_cmd.c`)

The layout is in formats.md. The parser checks every count and length against the input and
fixed caps (1,000,000 commands a list, 16 MB a string, 100,000 move-route steps) and copies what
it keeps, so a list does not depend on the buffer it came from.

## Commands built

| Code | What it does here | Source |
|---|---|---|
| 0, 99 | Blank / checkpoint: nothing | [V], [R] |
| -1 | End of the list | [V] |
| 103 | Comment: nothing | [V] |
| 104 | Sets the force-stop-choices flag other handlers read | [V] local lane |
| 105 | Sets the force-stop-message flag | [V] local lane |
| 106 | Debug text: logged, never a blocking dialog | [V] local lane, risks.md |
| 111 | Number condition and its branch | [R] |
| 112 | String condition and its branch | [R] |
| 121 | Set variable | [V] public format readers, manual |
| 170, 179, 498 | Endless loop, counted loop, loop end | [R], manual 04ev_control |
| 171, 176 | Leave the loop, start its next run | [R] |
| 172 | Stop this event | [R] |
| 180 | Wait | [R] |
| 210, 300 | Call a common event by number or by name | [R] |
| 212, 213 | Label, jump to label | [R] |
| 401, 402, 420, 421, 499 | Case markers, else, cancel, branch end | [R] |

## Call values (`core/src/values.c`)

A number in a command either is itself or names a variable, from the editor's manual,
help/06valueget.html [V]:

| Range | Names |
|---|---|
| below 1,000,000 | itself, a literal [V]; values below -999,999 are taken as literals too [M] |
| 1,000,000 + 10·E + X | map event E's self variable X (0-9) [V] |
| 1,100,000 + X | this map event's self variable X [V] |
| 1,500,000s | never written [V]; read as 0, logged once |
| 1,600,000 + X | this common event's self variable X (0-99; 5-9 hold strings) [V] |
| 2,000,000 + X | normal variable X [V] |
| 2,000,000 + 100,000·Y + X | spare variable block Y (1-9), X [V] |
| 3,000,000 + X | string variable X [V] |
| 8,000,000 + N | a random number 0 to N, freshly drawn [V] |
| 9,000,000 + X / 9,900,000 + X | system variable / system string X [V] |
| 15,000,000 + 100·C + X | common event C's self variable X [V] |
| 9,100,000s, 1,000,000,000 + | characters, databases: **not built yet**, read as 0, logged once |

Reads of anything outside the game's configured variables give 0 or "", and such writes are
ignored [M].

## Set variable, 121 (`core/src/calc.c`)

Layout from the public format readers (WodiLib, WolfTL, wolf-rpg-formats); operators and limits
from the manual, help/04ev_calc.html and help/04ev_value.html [V]. Four numbers: the target, two
operands, and a word holding the options (low byte), the assignment (next four bits) and the
operator joining the two operands (the four above). A fifth number repeats the assignment over
that many consecutive targets. ÷0 leaves the value as it was [M, by the manual's wording]. In a
common event with no option ticked and only plain variables involved, results wrap at 32 bits;
elsewhere they are held to ±2,000,000,000, or ±999,999 with that option [V].

## Conditions and branches: 111, 112, 401, 402, 420, 421, 499 (`core/src/flow.c`)

A list is flat. A condition opens a construct; at its own indent follow one case marker per
case, optionally an else marker, and a branch end; each case's commands sit one indent deeper.

- **Choosing** [R]: the cases are tried in order, and the first one whose test passes has its
  commands run. Tests are only tried until one passes. With none passing, the else commands run
  if there is an else marker, and otherwise the construct is skipped. The construct never
  reaches past its own branch end, so the commands after it run as usual.
- **Leaving** [R]: when a chosen body runs into the next marker of its construct, the rest of
  the construct is skipped.
- **Markers are matched by order, not by their numbers** [R]: real data gives case markers
  arguments that do not follow the case order.
- **111** [R]: the first number's low four bits are the case count (bit 4 says there is an
  else, which the markers show anyway). Each case is three numbers: a left call value, a right
  call value and a comparison: 0 `>`, 1 `>=`, 2 `=`, 3 `<=`, 4 `<`, 5 `≠`, 6 "any bit in common"
  [R]. **Unknown:** what the bits above the low byte of the
  comparison number mean; they are ignored. An unknown comparison is false and logged once.
- **112** [R]: the first number's low four bits are the case count. Each case word has the
  string variable in its low 24 bits, a flag in bit 24 that the compared-with string is another
  variable, and the comparison in its top four bits: 0 equal, 1 not equal, 2 contains, 3 starts
  with. A literal compared-with string is the command's string of the case's number; a variable
  one comes from the numbers after the case words, one per such case, in case order.
  **Unknown:** contains and starts with have no real call checked against them yet.
- **421** (cancel) is a choices marker [R]; inside a condition it behaves as an else.

## Loops: 170, 179, 498, 171, 176 (`core/src/flow.c`)

- **170** starts an endless loop; **179** a counted one, its count a call value. A count of 0
  or less skips the body entirely [R, manual 04ev_control].
- **498** ends one run: an endless loop starts again; a counted loop starts again while runs
  remain [R]. The loop end belongs to the nearest loop opener before it at its own indent.
- **171** leaves the loop it sits in. **176** ends the current run early, which uses up one run
  of a counted loop like reaching the end does [R, manual]. Both find their loop by matching
  openers and ends backwards, as they usually sit deeper inside a branch.
- A counted loop entered by a jump past its opener runs once [R, manual].
- An endless loop that never waits runs into the 5,000,000-commands-a-frame limit and stops as
  the runtime's "infinite loop" [V].
- Counts are kept per call, up to 32 counted loops at once; past that the oldest is forgotten
  [M: a safety cap; only a jump out of a loop leaves one behind].

## Stopping, waiting, labels: 172, 180, 212, 213 (`core/src/flow.c`)

- **172** ends the list it is in. In a called common event, the caller carries on after the
  call [R, manual "イベント処理中断" read by the reference].
- **180** waits that many frames: the event yields now and goes on in the frame after that many
  have passed; 0 does not yield [R]. **Unknown:** whether the count may be a call value; it is
  read as one, which leaves literals as they are.
- **213** goes on just past the first **212** of the same name in the same list [R]. A missing
  label is ignored and logged once.

## Common-event calls: 210, 300 (`core/src/flow.c`)

- **210**'s first number is 500,000 + the common event's number [R]. Anything else is a map
  event's page, which is **not built yet** (ignored, logged once). **Unknown:** whether that
  number may itself be a call value.
- **300** names the common event by its first string [R].
- Both take a status word second: its low four bits are how many number arguments follow, the
  next four how many string arguments, and bit 24 that a result is wanted, whose target call
  value follows the number arguments [R].
- Number arguments land in the callee's self variables 0-4, then 10 on, since 5-9 hold strings
  [R]. **Unknown:** past the fifth argument; real calls seen use at most four.
- String arguments are **not built yet**: they are not passed, logged once.
- The callee runs in the caller's thread, so its waits pause the caller too [R]. When its list
  ends, the result (its declared result self variable) is written to the target in the
  caller's context [R]. A call to a common event that does not exist is ignored and logged once.
- Calls nest at most 256 deep (above); past that the thread stops.

## Not built yet

Messages and choices (101, 102), pictures (150), sound (140), databases (250), 211 (reserved
calls), choosing and starting map event pages (maps load: formats.md, "Maps (.mps)"), string
arguments, and everything that draws or reads input. A command
with no handler is logged once and skipped (above).
