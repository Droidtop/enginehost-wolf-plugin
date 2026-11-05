# Event commands: the interpreter

Sources are marked as in the other notes: [V] read in a public source or traced by the local lane,
[M] our own reading, to be checked by the comparison runs.

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
| 0 | Blank: nothing | [V] |
| -1 | End of the list | [V] |
| 103 | Comment: nothing | [V] |
| 104 | Sets a flag other handlers read | [V] local lane |
| 105 | Sets a second such flag | [V] local lane |
| 106 | Debug text: logged, never a blocking dialog | [V] local lane, risks.md |
