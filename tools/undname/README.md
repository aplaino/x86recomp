# UnDecorateSymbolName reference check (dev-time, uses Wine)

FuBi (the game's scripting bridge) parses `dbghelp!UnDecorateSymbolName`
output (flags 0x800) for all 3230 exported functions. Our implementation
(src/runtime/hle/dbghelp.cpp) must match Microsoft's byte for byte, or
FuBi mis-registers functions and Skrit jobs fail silently (the hero
could not move and the farmer's conversation never started).

Reference: Microsoft's dbghelp.dll, which ships with the game, run
under Wine (behaviour reference only; nothing from it ships):

    i686-w64-mingw32-gcc -O1 tools/undname/undref.c -o /tmp/undref.exe
    awk -F'\t' '$2=="func"{print $3}' symbols/exports.tsv | sort -u > /tmp/names.txt
    cp "$HOME/Games/Heroic/Dungeon Siege/dbghelp.dll" /tmp/dbghelp_ref.dll
    wine /tmp/undref.exe 'Z:\tmp\dbghelp_ref.dll' /tmp/names.txt /tmp/ref.txt

Then undecorate the same names with recomp::hle::UndnameMsvc and diff (the
reference has CRLF line ends). Last result: 3230/3230 identical.
