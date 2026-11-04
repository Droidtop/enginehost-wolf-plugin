# Enginehost WOLF RPG Editor core

An interpreter of our own for WOLF RPG Editor games, as an Enginehost core
(Droidtop/tracker#858). No open-source Wolf runtime exists, and Wine is not a core. The plan is
Enginehost's `docs/new-engines/wolf.md`; this repository follows Enginehost's policy for cores
written from scratch (`docs/SPEC.md`, "Cores we write from scratch").

Branches, as for every Enginehost core (Enginehost `docs/plugin-lines.md`): `engine/0.1` is the
engine (this tree), `plugin-core` is the Enginehost wrapper, and `plugin/0.1` is composed from
the two and is the only branch that builds a bundle.

## What is here now

- `tools/survey/wolf_survey.py`: scans a folder of games in place and reports engine versions,
  packed or unpacked data, Pro games, and how often each event command is used. It writes only
  its report. Tests: `python3 -m unittest discover -s tools/survey`.
- `docs/behaviour/`: the behaviour descriptions the core is written from, and the risk list.

## What it reads

Only what the game itself reads: unpacked data, 2.x archives, and 3.x archives on the engine's
fixed key. Pro games and games built with a protect key are refused with a plain message.

## Licence

GPL-3.0 (`LICENSE`), as Enginehost's other own-interpreter core (CatSystem2).
