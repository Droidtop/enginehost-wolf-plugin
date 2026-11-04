# Behaviour descriptions

This folder is what the core is written from. Each page says what WOLF RPG Editor's runtime does,
in our own words: a file format, an event command, timing, drawing, sound or input. The core is
written from these pages and never from the official runtime's code. The owner: "We don't want to
use their code." (Enginehost docs/SPEC.md, "Cores we write from scratch".)

How a page is made:
- Evidence is the official runtime's behaviour as a local lane reads it (the static pass, done
  only in a local lane; its tools are not kept in this repository), the editor's manual, public format notes, and what games do on the rig. Cite it: the address or function a
  behaviour was seen at, the manual section, the game and checkpoint.
- Describe, do not transcribe: no decompiled code, no copied tables beyond the values a format
  needs (a magic number, a command code).
- Mark each claim **[V]** (checked), **[S]** (from a secondary source) or **[M]** (memory, to
  check). The comparison harness turns [M] into [V] or into a fix.

Pages:
- `formats.md`: the files a game ships and how they are laid out.
- `risks.md`: everything the runtime does that could be abused, and the core's answer to each.
