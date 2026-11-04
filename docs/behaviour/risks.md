# Risks

Every runtime behaviour that could be abused, and the core's answer (Enginehost docs/SPEC.md,
"Cores we write from scratch", step 2). Filled from a local lane's static pass and the survey. Status today: candidates from memory, none confirmed.

| Behaviour | Seen | Core's answer |
|---|---|---|
| String commands reading or writing text files by path | [M] | Only inside the game folder and the save folder; any other path behind a Fibrewall permission, off by default |
| Opening a URL or a file through the system shell | [M] | Behind a Fibrewall permission, off by default; asks the person each time |
| Calling an external DLL (3.x) | [M] | Not implemented: a core cannot run Windows code. The game is told the call failed |
| Screenshots and writes outside Save/ | [M] | Writes go to the save folder Enginehost gives the core |
| Network access | [M] | None. The core has no network access (isolated process) |
| Lengths and counts in the game's data | [V] (every format) | Every reader is bounds-checked; malformed data fails the load with a message, never reads past a buffer |
