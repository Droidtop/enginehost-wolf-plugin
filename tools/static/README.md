# Static pass

Step 1 of the from-scratch policy: read the official runtime without running it.

On a local rig session, with Ghidra 11 and its JDK, for each `Game.exe` / `GamePro.exe` the
survey found (one per engine version is enough):

    analyzeHeadless /tmp/wolf-ghidra wolf -import "/path/to/Game.exe" -readOnly \
        -scriptPath tools/static -postScript wolf_static.py /tmp/wolf-static-2.281.json

`-readOnly` keeps the program out of the project; nothing is written next to the game. The
report lists imports by DLL, the risky ones with the functions that call them, and the strings
that name files, URLs and DLLs. A person then reads those functions in Ghidra and writes what
they do into `docs/behaviour/`, in their own words. The report itself may be attached to #858;
decompiled code may not be committed here.
