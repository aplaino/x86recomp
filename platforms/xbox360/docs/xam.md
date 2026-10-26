# xam: behaviour the runtime reimplements

What xam (2.0.17559.0, which the console's system update ships
unencrypted) does, as far as the runtime depends on it, and where each
fact comes from: xam's own code first, then what titles read, then
Xenia's and ReXGlue's implementations (BSD-3). "Console check" marks what
only a console can settle.

## The console
- XGetAVPack and XGetVideoMode are each a single branch to the kernel's
  HalGetCurrentAVPack and VdQueryVideoMode, so they return what those do
  (docs/xboxkrnl.md, "Settings and the display").
