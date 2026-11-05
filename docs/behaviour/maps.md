# Maps at run time: which page an event shows, and the runs that start by themselves

Sources are marked as in `commands.md`. [R] is behaviour learned by reading
take-cheeze/rpg-maker-clone's `mruby-wolf` (MIT) at commit 75b1c34 (its interpreter's map-event
update and page choice), as the owner allowed on #858: read only, never built, run, copied or
vendored. The reference itself cites the editor's manual (help/04eventwindowB.html) where it
has one. [M] is our own choice where no source settles it. **Unknown** marks what is open.
Built in `core/src/wolf_maprun.c`; how pages are stored is formats.md, "Maps (.mps)".

## Which page an event shows

- **A page qualifies** when every one of its *used* conditions holds [R; manual 04eventwindowB
  as cited by the reference]. A condition is used when bit 0 of its byte is set; the other low
  bits are ignored (their meaning is Unknown). A page with no used condition always qualifies.
- **A condition** compares two call values: the condition's variable on the left and its value
  on the right, both read the way any command's operands are (commands.md, "Call values"), so a
  number from -999,999 to 999,999 is itself and anything else names a variable [R]. The comparison is
  the byte's high four bits, numbered as command 111 numbers them (0 `>`, 1 `>=`, 2 `=`, 3 `<=`,
  4 `<`, 5 `≠`, 6 any bit in common); an unknown one is false and logged once [R]. The core uses
  the one comparison for both (`wolf_compare`).
- **Context** [R]: conditions are read as the event itself, so 1,100,000 + X is that event's own
  self variable X.
- **Several qualifying pages:** the **last** one, in the order the editor stores them, is shown
  [R]. The reference notes the manual only says a page needs all its own conditions; last-wins
  is the convention the reference carries over from the RPG Maker 2000/2003 engines it also
  reads. **Unknown:** whether Wolf's runtime really uses this precedence; to be checked on the rig.
- **No qualifying page:** the event shows nothing and runs nothing [R].
- Wolf has no separate switches: an on/off condition is a variable compared with 0 or 1, so the
  "switch" case is the variable case.

## Runs that start by themselves

Each frame, `wolf_maprun_step` goes over the current map's events in the order the file stores
them [R]:

1. find the event's page as above;
2. if that page is **automatic** (trigger 1) or **parallel** (2): when the event has no run,
   start one on that page's list with the event's self variables, and step it at once in the
   same frame; when it has one, step it [R];
3. a run whose list ends (or that ends on 172) is dropped at once [R].

Each step is one frame of the interpreter, under the dispatcher's rules (commands.md): up to
5,000,000 commands, stopping early when a command yields (a wait, a message).

- **An automatic page restarts** on the frame after its run ends, as long as it is still the
  event's page [R]. The reference calls this a simplification: it does not wait for its
  condition to become true again. **Unknown:** whether Wolf restarts it the same way, or only
  on a fresh edge; games normally end an automatic page by changing the variable it depends on,
  which works either way.
- **A run is tied to the event, not to the page** [R]: if the event's page changes to another
  automatic or parallel page while a run is going, the run carries on with the list it started
  on; a new one starts only once it ends. While the event's page is not automatic or parallel
  (or none qualifies), its run is **not stepped**: it stays where it stopped and goes on once
  such a page shows again [R]. **Unknown:** whether Wolf finishes, pauses or drops such a run. A
  paused automatic run keeps the map held (below), so a game relying on the other behaviours
  would freeze here; watch for it on the rig.
- **Holding the map** [R; the reference cites the manual's "Auto Start excludes other events"]:
  while any automatic page's run is going, `wolf_maprun_blocking` is true; the hero and the
  hero-checked and touch pages must wait. Parallel runs never hold anything. Nothing reads this
  yet (hero movement and the checked and touch triggers are not built); the reference likewise
  keeps stepping the other runs while one holds.
- **Checked and touch pages** (triggers 0, 3, 4) never start by themselves [R]; starting them
  needs the hero and is not built.
- **A run that fails** (over the frame budget, calls too deep, a refused command) is logged
  and dropped, and that event is not started again until the map is entered again [M: the
  reference has no rule; restarting it every frame would repeat 5,000,000 commands a frame].
  An event whose id has no self-variable slots is logged and never run, the same way.

## Changing map

- **Entering a map drops every run of the map before** [M, the coordinator's directive on
  #858]. The reference keeps its list of runs across a map change and finds a run by event id,
  so after a move a run of the old map could be stepped under a new map's event of the same id;
  we do not copy that. A map entered again starts its automatic and parallel pages from their
  first command [R: the reference says revisited maps' events "restart fresh"].
- **Variables are not touched** by a map change, self variables included. **Unknown:** whether
  Wolf clears a map's self variables when the map is left or entered; it is not done.
- A map change from inside a frame (a move command) is refused while the frame is going
  (`WOLF_MAPRUN_E_BUSY`); the move command is not built, and will have to take effect between
  frames.

## Not built yet

Hero position and movement, event movement and routes, the checked and touch triggers,
common events that start by themselves (automatic and parallel common events: the reference
runs them before the map's events each frame), and drawing. Pages' graphics, directions and
options are loaded (formats.md) but unused.
