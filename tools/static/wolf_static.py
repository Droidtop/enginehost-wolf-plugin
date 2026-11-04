# Ghidra headless post-script: the static pass's inventory of an official Wolf runtime.
#
#   analyzeHeadless <project-dir> wolf -import <Game.exe> -readOnly \
#       -scriptPath tools/static -postScript wolf_static.py <out.json>
#
# Runs inside Ghidra (Jython 2.7, so no f-strings). It records what the program calls, never
# what its code is: every imported function, grouped by DLL; the strings that name files, URLs,
# DLLs and DX Library; and, for each import in a risk category, the functions that call it, so a
# reader knows where to look. People write docs/behaviour/ from this inventory and from reading
# in Ghidra; nothing from the program's code goes into the core (the from-scratch core policy,
# Enginehost docs/SPEC.md, "Cores we write from scratch").
#
# Risk categories are the policy's list: file access outside the game's folder, network access,
# starting processes, loading DLLs or plugins. Parsing that trusts the game's data cannot be
# found by imports; the reader marks it in docs/behaviour/risks.md.
import json
import re

from ghidra.program.model.symbol import SymbolType

RISK = {
    "network": re.compile(r"^(WSA|socket|connect|send|recv|gethostby|getaddrinfo|Internet|Http|WinHttp|URLDownload|InternetOpen)", re.I),
    "process": re.compile(r"^(CreateProcess|ShellExecute|WinExec|system$|_spawn|_exec)", re.I),
    "library": re.compile(r"^(LoadLibrary|GetProcAddress|LdrLoadDll)", re.I),
    "file": re.compile(r"^(CreateFile|DeleteFile|MoveFile|CopyFile|CreateDirectory|RemoveDirectory|FindFirstFile|SetCurrentDirectory|GetTempPath|SHGetFolderPath|SHGetSpecialFolder|SHFileOperation|fopen|_wfopen)", re.I),
    "registry": re.compile(r"^Reg(Open|Create|Set|Delete|Query)", re.I),
    "clipboard": re.compile(r"^(OpenClipboard|SetClipboardData|GetClipboardData)", re.I),
}
STRING_HINTS = re.compile(r"(https?://|\.dll\b|\.exe\b|DxLib|DXArchive|\\\\|[A-Za-z]:\\|Save|\.sav\b|\.wolf\b|\.dat\b|\.mps\b)", re.I)


def imports(program):
    out = {}
    st = program.getSymbolTable()
    for sym in st.getExternalSymbols():
        if sym.getSymbolType() not in (SymbolType.FUNCTION, SymbolType.LABEL):
            continue
        lib = sym.getParentNamespace().getName()
        out.setdefault(lib, []).append(sym.getName())
    for lib in out:
        out[lib].sort()
    return out


def callers(program, name):
    fm = program.getFunctionManager()
    seen = set()
    for sym in program.getSymbolTable().getExternalSymbols():
        if sym.getName() != name:
            continue
        for ref in sym.getReferences():
            for thunk_ref in [ref]:
                f = fm.getFunctionContaining(thunk_ref.getFromAddress())
                if f is None:
                    continue
                # Calls go through an import thunk; report who calls the thunk.
                if f.isThunk():
                    for r2 in program.getReferenceManager().getReferencesTo(f.getEntryPoint()):
                        g = fm.getFunctionContaining(r2.getFromAddress())
                        if g is not None:
                            seen.add("%s@%s" % (g.getName(), g.getEntryPoint()))
                else:
                    seen.add("%s@%s" % (f.getName(), f.getEntryPoint()))
    return sorted(seen)


def strings(program):
    out = []
    listing = program.getListing()
    for data in listing.getDefinedData(True):
        if not data.hasStringValue():
            continue
        v = data.getValue()
        if v is None:
            continue
        v = unicode(v)
        if STRING_HINTS.search(v):
            out.append({"at": str(data.getAddress()), "text": v[:200]})
    return out


def main():
    args = getScriptArgs()
    out_path = args[0] if args else "wolf-static.json"
    program = currentProgram
    imp = imports(program)
    risky = {}
    for lib, names in imp.items():
        for n in names:
            for cat, rx in RISK.items():
                if rx.search(n):
                    risky.setdefault(cat, []).append({"import": "%s!%s" % (lib, n), "callers": callers(program, n)})
    report = {
        "program": program.getName(),
        "imageBase": str(program.getImageBase()),
        "md5": program.getExecutableMD5(),
        "functions": program.getFunctionManager().getFunctionCount(),
        "imports": imp,
        "risk": risky,
        "strings": strings(program),
    }
    f = open(out_path, "w")
    try:
        f.write(json.dumps(report, indent=1, sort_keys=True))
    finally:
        f.close()
    print("wolf_static: %d imports in %d DLLs, %d risky, report in %s" % (
        sum(len(v) for v in imp.values()), len(imp), sum(len(v) for v in risky.values()), out_path))


main()
