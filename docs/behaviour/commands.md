# Event commands: the interpreter

Sources are marked as in the other notes: [V] read in a public source or traced by the local lane,
[M] our own reading, to be checked by the comparison runs. [D] is a pattern counted in the owner's
games' own data files, read in place (the survey games of #858: Asuka Virgin Idol Debut 3.135,
Living With Sister 3.191, The Clumsy Witch Seris 3.714); it shows how real commands are laid
out, not what the runtime does with them. [R] is behaviour learned by reading
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
| 101 | Show a message, and wait for it to close | manual 04ev_text, [D] |
| 102 | Choices, and their branches | manual 04ev_select, [D], [R] |
| 104 | Force a choice list shut (its forced branch); sets the force-stop-choices flag | manual 04ev_select |
| 105 | Force a message shut; sets the force-stop-message flag | manual 04ev_text |
| 150 | Pictures: show, move, erase, delay reset, over time, one or a range | manual 04ev_picture, [D], [R] |
| 140 | Sound: play, stop, preload, free BGM, BGS and SE | manual 04ev_sound, [D], [R] |
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
| 250 | Database: read a field into a variable, or write one | manual 04ev_db, [D], [R] |
| 122 | Set string: =, +=, lines, characters, remove, replace, up to, after | manual 04ev_string, [D] |
| 123 | Key input: the basic keys, now or waiting for a press | manual 04ev_keyinput, [D], [R] |

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
| 1,000,000,000 + 1,000,000·A + 100·B + C | user database (DataBase), type A, data B, field C [V] |
| 1,100,000,000 + ... | changeable database (CDataBase), same layout [V] |
| 1,300,000,000 + ... | system database (SysDataBase), same layout [V] |
| 1,200,000,000s, 1,400,000,000 up | name no database [V, by omission]; read as 0, logged once |
| 9,100,000 + 10·Y + X | map event Y's character value X [V]: see below; **not built yet** |
| 9,180,000 + 10·Y + X | the hero (Y = 0) or party member Y, value X [V]; **not built yet** |
| 9,190,000 + X | this event's character value X [V]; **not built yet** |

Character values X [V, 06valueget]: 0 and 1 map X and Y in tiles; 2 and 3 X and Y in half
tiles; 4 the event's height in pixels; 5 its shadow number (a party member's -1 takes the
hero's); 6 its direction (1-9); 7 and 8 its pixel offsets X and Y; 9 its character image (a
text). Writing a position makes the character walk there at its move speed [V]. Until
characters are built these read as 0 and writes are ignored, logged once.

Database call values read and write the field in place: a number field through the number
calls, a text field through the string calls. A field of the other kind, or a type, data or field
the database does not have, reads 0 or "" and ignores writes, logged once [M]. Writes to the user
and system databases are allowed and last until the game ends [M: the manual only calls these
"read or write"; **Unknown** whether the runtime refuses them]. Only the changeable database is
expected to go into saves (the save format is not built yet).

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

## Database: 250 (`core/src/db.c`)

What it does is from the manual, help/04ev_db.html [V]. The argument layout was read from
take-cheeze's mruby-wolf [R] and checked against all 17,291 commands 250 in the survey games [D]:

- Five numbers in nearly all real commands (17,033 [D]): the type, the data and the field
  (each a call value), a packed word, and the value or target (a call value). 258 commands have
  four numbers and write the command's first string, a literal text [D].
- The packed word: the low byte holds the assignment in its high four bits, numbered as
  121's (0 =, 1 +=, 2 -=, 3 ×=, 4 ÷=, 5 %=, 6 raise to, 7 lower to; 0 to 7 are seen [D]) and,
  in bit 1, "the value is the literal text" (set on exactly the four-number commands [D]).
  The next byte holds the database in its low four bits (0 changeable, 1 system, 2 user) and the
  operation in its high four (0 write, 1 read). These four combinations are the only ones in
  the data [D]. The third byte's bits 0-2 say the type, data and field are named by the
  command's strings 2, 3 and 4 (the manual's 名前 boxes).
- **Read** combines the field with the target ("target how= field") and stores the result in the
  target. **Write** combines the field with the value ("field how= value") and stores the result
  in the field [R].
- **Text fields** take = and += (joined); any other assignment is taken as = and logged once
  [M: the reference sees only = and += on texts, and the manual names no others].
- **Number results** are held to ±2,000,000,000, as 121 outside the common-event wrap [M].
- **Past the end** [V, manual]: a field number of 100 or more moves on to the next data, and a
  data number of 10,000 or more moves on to the next type. 250 is one of the two commands the
  manual allows this for.
- **Name boxes**: when a name bit is set and its string is not empty, the core looks the name up
  in that database (type by name, then data and field within that type), and the name decides
  the number. A name the database does not have leaves the stored number, logged once [M]. In
  the reference's sample game the stored numbers always matched the names [R]. The manual says
  data names are matched against the current name from 3.566 on, and the core matches each
  row's name as loaded. **Unknown:** how a type whose data names come from its first text field
  (the 最初の文字列 setting) is stored. Until that is known, such types match the stored row
  names.
- **Refused, logged once, and skipped** (the list carries on): a database other than 0-2, an
  operation other than read and write, a negative or missing type, data or field, a literal
  text written into a number field, a database the game did not load, and too few numbers.
- **Unknown:** what bit 0 of the low byte means (ignored, logged once; it is 0 in every real
  command [D] [R]).

**Not built in 250**: the manual's other database operations (counting data and fields,
names to numbers and back, initialising a type or a data, inserting, removing, copying and
sorting data, XY arrays, CSV in and out). The survey games carry none of these in 250 [D].
They appear under separate codes instead, laid out as below [D], and stay unhandled until
their meaning is settled from the manual and a comparison run:

| Code | Real shape [D] | Likely, from the manual's feature list and the strings | Uses |
|---|---|---|---|
| 251 | five numbers (type, 0, 0, word, number) and four strings, the first a `.csv` path | CSV in/out (04ev_db item 5) | 23 |
| 252 | five numbers, word bytes 0x12/0x13 | data insert/remove/copy (item 4) | 4 |
| 255 | five numbers, word 0x10300/0x11300, a type name | data sort or counts | 50 |
| 256, 257 | one each | Unknown | 2 |

The CSV paths are game-relative and use `\cself[]`/`\sys[]` escapes. The manual refuses
`//`, `\\`, `..`, `./`, `.\`, `%`, `:`, `*`, `?`, `"`, `<`, `>` and `|` in them (Ver 3.00 on),
and the core will apply that rule to every file name a command gives (risks.md).

## Text: the substitution codes (`core/src/text.c`)

From the manual, help/06specialword.html [V]. Text in commands (122's text, and later messages
and string pictures) can hold codes that stand for a value:

| Code | Stands for |
|---|---|
| `\v[X]`, `\v1[X]` to `\v9[X]` | normal variable X; spare variable block 1-9, X |
| `\self[X]` | this map event's self variable X |
| `\cself[X]` | this common event's self variable X (5-9 are texts) |
| `\sys[X]`, `\sysS[X]` | system variable X; system string X |
| `\udb[A:B:C]`, `\cdb[...]`, `\sdb[...]` | user, changeable, system database: type A, data B, field C; `[A:B]` the data's name; `[A]` the type's name |
| `\s[X]` | string variable X |

- **Order** [V]: the manual ranks them `\v` < `\v?` < `\self` < `\cself` < `\sys` < `\udb` <
  `\cdb` < `\sdb` < `\s` < `\sysS`, and a code's brackets may hold any code ranked below it.
  So `\cself[\v[12]]` works, and `\v[\cself[2]]` does not (the manual: "not processed
  normally"). The core does one pass per code in that order. Each pass replaces every code
  whose brackets hold only a number (for the databases, up to three joined by `:`). By the time
  a code's pass comes, any lower code inside its brackets has already been replaced. A code
  whose brackets hold anything else is left as written; that's what happens to
  `\v[\cself[2]]` [M: what the runtime shows for it is **Unknown**].
- Up to 100 levels of nesting work from 3.50 on [V]. The pass order covers any depth.
- `\\` is a backslash, never the start of a code. It is left as `\\` for whoever shows the text
  [M]. Display codes (`\c[]`, `\f[]`, `\!`, `\r[,]`, `\i[]`, ...) are left for messages and string
  pictures.
- A value put in by one pass is not read again by the same pass. A later pass may still find
  codes inside a string variable's value [M: whether the runtime re-reads replaced text is
  **Unknown**].
- A database code naming a type or data the database lacks gives "" [M]. A negative number, or
  more than three, leaves the code as written.
- **Not built:** `\xy[]` and `\xyN[]` (XY arrays). Left as written, logged once.
- Results are capped at 4 MB and cut there, logged once [M: a safety cap].

## Set string: 122 (`core/src/strings.c`)

What each assignment does is from the manual, help/04ev_string.html [V]. The argument layout was
read from all 3,447 commands 122 in the survey games [D] (the reference only models plain
assignment):

- Numbers: the target (a string call value), a packed word, and, for a variable source, the
  string variable to read. Real commands have 2, 3 or 4 numbers and 0, 1 or 2 texts [D].
- The packed word's low four bits are the source [D]: 0 the command's first text, with its
  codes replaced (above); 1 a string variable (third number); 2 a variable holding a string call
  value (third number, "ロード位置を変数で指定"); 3 keyboard input (the third number is the
  length, 30 and 10 in real commands).
- Bit 4 (0x10): the target itself is held by a variable ("代入先を変数で指定") [D, matches the
  manual's option].
- The second byte is the assignment [D, numbering matched to the manual's list by what the
  real texts are]:

| Assignment | Does | Real texts that fix it [D] |
|---|---|---|
| 0 = | target = right side | the 2,329 plain texts |
| 1 += | target = target joined with the right side | |
| 2 copy top line | target = the right side's first line | from string variables only |
| 3 cut top line | as 2, and a variable right side loses that line and its line break | |
| 4 cut one character | target = the right side's first character; a variable right side loses it | |
| 5, 6, 7 | read a file, write a file, list a folder | `Data/…/info.ini`, `…_改行調整.txt`, `Data/改変テキスト/` |
| 8 remove all | every occurrence of the right side removed from the target | `Data/` taken out of paths |
| 9 replace | every occurrence of the first text replaced by the second, in the target | `\n` → a line break |
| 10 up to | target = what comes before the right side; `<<NotFound>>` when absent | |
| 11 after | target = what comes after the right side; `<<NotFound>>` when absent | |

- A line ends at `\n` or `\r\n` [M]. "One character" is one character of the game's encoding
  (UTF-8 for 3.x, Shift-JIS for 2.x, from Game.dat's marker), or one whole display code: the
  manual's `\A+` and `\c[1]` go in one cut, and of `\r[text,ruby]` only `\r[` goes [V].
- A text right side is only read, never cut [V: the manual cuts "the right side" only when it
  is a variable].
- 10 and 11 are matched to the manual's "から指定文字まで切り出し" and
  "の指定文字以降を切り出し" by list order. A real `[\cself[1]]:` under 10 fits "up to" [M].
- In 9 the second text has its codes replaced too [M].
- **Not built, logged once and skipped** (the list carries on):
  - keyboard input (source 3);
  - the file assignments 5-7. They need the file rules in risks.md first: game folder only, the
    manual's refused sequences, and writes to the save directory only;
  - the forms with a third byte (4 in all real cases [D]). These carry the regular-expression
    texts and the `<<…>>` hidden codes (help/04ev_string_specialcode.html), and the meaning of
    each assignment number under that byte is **Unknown** until a comparison run;
  - assignments past 11. The manual's newer entries (cut the last character, cut up to with
    the rest kept, Base64, remove display codes) must be among them; their numbers are
    **Unknown**.

## Messages and choices: 101, 102, 104, 105 (`core/src/ui.c`)

The plugin draws the window; the core says what to show and waits for the answer
(`wolf_ui` in vm.h: `message`, `choices`, `close`; answers through `wolf_vm_message_closed` and
`wolf_vm_choice_made`).

- **101** [V, manual 04ev_text]: one text (all 43,207 real ones [D]), with the substitution
  codes replaced (see "Text"). Display codes (`\c[]`, `\f[]`, `\!`, `\.`, `\r[,]`...) are passed
  through for the drawing to act on. A first line that is "@" and a number alone sets system
  variable 0 (the face graphic number) and is not shown (13,611 real messages start that way
  [D]). The thread waits in the command, frame after frame, until the message is closed.
- **102** [V, manual 04ev_select; layout [R], checked on all 941 real ones [D]]: one number,
  with the choice count in its low four bits, and one text per choice.
  - **Cancel**, bits 4-7: 0 goes to a separate cancel branch (421). The 304 real lists with 0
    carry exactly the 304 real 421 markers [D]. 1 disables the cancel key. 2 + n acts as if
    choice n were picked; a value naming no choice is taken as disabled [M].
  - **Extra branches**, bits 8-10: left key, right key, forced stop. Each set bit has its own
    402 marker after the choices, in that bit order. The 4 real lists with left/right bits
    carry exactly the 6 real 402s [D]; the order is [M]. Left/right are not built, so those
    branches are never taken (logged once).
  - Choice texts have their codes replaced. A text that ends up empty is not shown but keeps
    its place, so choice n is still the nth 401 marker [V]. When every text is empty, the whole
    command is skipped [V].
  - The answer goes to the body after the nth 401. An answer the list cannot have (out of
    range, or a cancel while cancel is disabled) skips past the branch end, logged once [M].
- **104** [V, 04ev_select]: a choice list that is up is taken down and its forced branch is
  taken, or the construct is left when it has none [M]. **105** [V, 04ev_text]: a message that
  is up closes as if the player had closed it. The manual says both only work from parallel
  events; the core lets any thread do it [M].
- **One window at a time** [M]: a second thread that reaches 101 or 102 while one is up waits
  its turn.
- **Headless** (no UI set): a message is logged and closes at once; a choice list takes its
  first shown entry, logged. That keeps tests and the AVD's first runs moving.
- **Unknown:** how the runtime lays out and pages text, and the key timing. That belongs to
  the drawing, built next, and to comparison runs.

## Pictures: 150 (`core/src/picture.c`)

The core keeps each picture's state and moves it frame by frame. The plugin calls
`wolf_vm_pictures_tick` once a frame and draws `wolf_vm_picture_at(0..count-1)`, lowest number
first. What each part does is from the manual, help/04ev_picture.html [V]. The layout of the
first eleven numbers is from mruby-wolf [R]; the rest comes from all 29,452 real commands in the
survey games [D]. Real commands have 2, 3, 4, 6, 11-15, 18, 19 or 25 numbers, and a shorter
one simply stops early [D].

**The options word** (first number): bits 0-3 the operation (0 show, 1 move, 2 erase, 3 delay
reset); 4-6 what is shown (0 a file, 1 a file named by a string variable, 2 a text, 3 a window
built from a base image, 4 the same named by a variable); 8-11 the blend (0 normal, 1 add, 2
subtract, 3 multiply, 15 keep; others are taken as normal); 12-15 the anchor (0 top left, 1
centre, 2 bottom left, 3 top right, 4 bottom right, 5 top centre, 6 bottom centre; all seven
occur [D], and the manual lists seven); 16-19 = 1 relative place [D, below]; 20-23 the zoom
mode (0 one value, 3 width and height apart, 4 keep); 24 a range; 26 free transform.

**Show and move**, by position: 1 the picture number; 2 the process time in frames; 3, 4 the
split (a window's width and height); 5 the piece shown; 6 opacity (0-255); 7, 8 X and Y; 9
zoom % (the width when apart); 10 angle in tenths of a degree; 11 the string variable naming
the file (types 1 and 4); 12 a colour word, 0x2000000 when 15-17 apply and 0x1000000 when the
colour is kept [D: shows carry the first, moves the second]; 13 the delay in frames before the
command acts; 14 the range's last number; 15-17 colour R, G, B in % (100 normal); 18 the zoom
height when apart. Any number may be a call value. -1,000,000 means "the same as now" (the
manual's 同値).

- **Relative**, bits 16-19 = 1 [D]: X and Y add to where the picture is. Of the moves with this
  bit, 5,022 positions are 0 and 997 are small offsets (60 or less). Moves without it mostly
  carry variables and screen positions. That fits the manual's 相対モード and nothing else.
- **Erase**: 2 the process time, 3 the delay, 5 the range's last number (in the six-number
  form). **Delay reset**: 2 the range's last number.
- **Over time** [V: the manual's 処理時間]: values move in a straight line from where they are
  to the new ones over the process time, one step a frame [M: straight-line steps]. A new
  picture shown with a process time fades in from nothing at its new place [M]. An erase with a
  process time fades out, then goes [M]. Showing a picture again replaces its image and moves
  from where it is (the manual: show on a shown number "acts as move").
- **Delay** [V: 発動ディレイ]: the command acts after that many frames, with the values worked
  out when it ran [M]. A delay reset drops the waiting ones for its numbers. The manual says
  delayed commands are not saved.
- **Text pictures and file names** have their substitution codes replaced (file names from
  3.345 on [V]); display codes stay for the drawing. The special file names `<SCREENSHOT>`,
  `<MAP_SCREENSHOT>`, `<CUT/...>`, `<SQUARE>`, `<GRADX-...>` and `<LINE...>` (window type) are
  passed to the plugin as written; drawing them is the plugin's job.
- **Draw order** [V]: higher numbers above. The manual places the map, characters, fog and the
  message window between number bands (-400,000 / -300,000 / ... / 100,000), differently under
  "picture behaviour Ver1" and "Ver2". The plugin draws by those bands; which Ver a game uses is
  a Game.dat setting, **Unknown** where.
- Caps (ours): 65,536 pictures shown and 65,536 delayed commands; a range touches at most
  10,000 pictures; times and delays at most 1,000,000 frames.
- **Not built:** free transform (the 25-number form, 19 real commands [D]), drawn as a plain
  rectangle and logged once; "link to scroll"; a move's own easing if the runtime has one
  (**Unknown**); saving pictures.

## Sound: 140 (`core/src/sound.c`)

The core works out what to play and hands a request to the plugin (`wolf_audio` in vm.h). What
each part does is from the manual, help/04ev_sound.html [V]. The first number's layout is from
mruby-wolf [R], checked on all 5,306 real commands [D]:

- **First number**: bits 0-3 the process (0 play, 1 preload, 3 free what is not playing; the
  manual's fourth, manual free, was removed in 2.20); bits 4-7 the kind (0 BGM, 1 BGS, 2 SE);
  bits 8-23 a signed system-database entry (-1 is "(停止)", stop); bits 24-31 the source (0 a
  system-database entry, 1 an entry named by a variable, 2 a file). A stop is stored
  sign-extended over the source byte, so real stops read -256, -240 and -224 for BGM, BGS and
  SE [D]. The core takes 0xFF with entry -1 as a stop.
- **Then**: 1 the fade in frames (BGM/BGS; real values 10, 30, 60 [D]); 2 the start position
  in milliseconds (BGM/BGS, the manual's 途中再生); 4 volume % and 5 frequency % (file source
  only; real values include 100, 50 and 0 [D]). A frequency of 0, seen in real SE commands, is taken
  as 100 [M]. **Unknown:** numbers 3, 6 and 7. The manual's other fields are pan (-255 to 255,
  3.50 on), loop start, the MIDI key and the BGS channel, but which number holds which isn't
  settled; they're not passed on yet.
- **System-database entries** come from types 1 (BGM), 2 (BGS) and 3 (SE) [V, manual]. The
  core takes the row's first text field as the file and its first two number fields as volume
  and frequency [M: field order not checked against a real system database]. An entry the game
  lacks is skipped, logged once. Where an entry named by a variable keeps its variable is
  **Unknown**; the core reads number 3 [M]. No real command uses that source [D].
- File names have their substitution codes replaced (`\cself[9]` is the common real form [D]).
  An empty name stops [M].
- Without audio set, requests are logged at debug level.
- **Not built:** the SE delay and "stop also clears delayed SEs" from the manual, BGS
  channels, pan, loop start, MIDI keys, and saving the playing BGM/BGS.

## Key input: 123 (`core/src/keys.c`)

The player's keys come in once a frame through `wolf_vm_input` (confirm, cancel, sub and the four
directions; the plugin maps the person's controls onto them). The core keeps what is held and
what was newly pressed this frame. The built-in message window answers first and takes the
presses it used, so the confirm that closes a message does not also answer a 123 in the same
frame [M].

What 123 does, from the manual (help/04ev_keyinput.html) [V], its layout read with mruby-wolf [R]
and checked against the owner's games [D]:

- **Numbers**: 0 the target the result goes into; 1 the options. A third number appears in the
  forms that read one key of the keyboard or a pad by code (e.g. options 0x1180 with key 142,
  74 uses [D]).
- **Options, low byte**: bits 0-3 the directions that count (0 none, 1 all four, 7 up and down,
  8 left and right); 0x10 confirm, 0x20 cancel, 0x40 sub; 0x80 wait until one is pressed [R].
  The commonest real forms are 0x11 (153 uses), 0x91 (122) and 0x10 (50) [D].
- **The result** is the manual's code: confirm 10, cancel 11, sub 12, a direction as the numeric
  keypad (2 down, 4 left, 6 right, 8 up), 0 for none [V]. With several keys at once the first in
  that order wins [M: the runtime's order is **Unknown**].
- **Without the wait** the target gets what is held now [V]. **With it** the event waits for a
  new press of one of the keys asked about [M: a key still held from before does not answer],
  and that press answers only this wait [M].
- **Unknown, logged once:** other direction sets (2-6, 9-15; read as all four), option bits above
  the low byte (0x100, 0x200, 0x1000, 0x20000 appear in real commands [D]; the manual's other
  modes, new-press-only, release-only and held-frames, are probably among them; read as the
  basic form), and the key-code forms (not built: the target gets 0 and the event goes on).

## Not built yet

The database operations
beyond read and write (251, 252, 255-257), 211 (reserved calls), 1000 (24 uses in one 3.x game
[D], one number and a `.jpg` path under `Save/` or `Data/`: by the strings, saving the screen to a
picture file; Unknown), starting map event pages by the hero (automatic and parallel pages run: maps.md), string
arguments, and the input commands beyond 123. A command
with no handler is logged once and skipped (above).
