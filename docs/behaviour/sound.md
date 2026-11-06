# Sound

How the core plays what command 140 asks for (commands.md, "Sound", has the command's layout).
The plugin decodes files to PCM; `core/src/wolf_mixer.c` mixes them into the stereo stream the
plugin writes to Enginehost's audio ring. Markers as in the other pages.

- **BGM and BGS** loop, one of each [V, manual 04ev_sound.html]. A new one replaces what is
  playing. The **same** file asked for again keeps playing and only takes the new volume and
  frequency [V: "曲自体はそのまま再生され続けます"]. **BGS channels** (3.50 on) are not built:
  one BGS [M].
- **Sound effects** play once. Up to 32 play at once, and the oldest gives way to a new one [M:
  the runtime's own limit is **Unknown**]. A stop ends them all [V: the manual's SE "(停止)"].
- **Volume** is a percent of the decoded level; voices add, and the sum is clamped to 16 bits
  [M: DX Library's mixing curve is **Unknown**].
- **Frequency** is a percent of the playback rate, so pitch and speed change together [V:
  "周波数"], held to 10-400% [M: a cap of ours]. Sounds at another sample rate are resampled
  linearly [M].
- **Fades** are in game frames (1/60 s) [V: "フレーム数で指定"]: a play with a fade starts silent
  and rises linearly; a stop with a fade falls linearly, then the voice ends [M: linear].
- **Start position** (BGM/BGS) in milliseconds [V: 途中再生]. A loop goes back to the start, not
  to the manual's loop point, because the loop-point number isn't located yet (commands.md).
- **Not built:** MIDI (the runtime uses `GuruGuruSMF4.dll`, an x86 Windows DLL the core never
  loads; a SoundFont synth would be needed), pan, loop points, the MIDI key change, SE delays.
