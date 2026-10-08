# Contributing

Bug reports, ideas and pull requests are welcome.

## Building
```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
./build/diskforge
```
Needs `qt6-base`, `qt6-svg`, `udisks2` and `cmake`.

## Before sending a pull request
DiskForge changes real disks, so every change to `src/udisks.cpp` has to pass the self-test:
```bash
sudo ./build/diskforge-selftest        # every operation on a throwaway loop-device image
./build/diskforge-selftest --guard     # the system disk is refused (run as yourself)
```
For UI changes, render the dialogs and look at them:
```bash
QT_QPA_PLATFORM=offscreen ./build/diskforge-preview out/
```

## Rules for disk code
- Never open block devices directly. Every change goes through UDisks2.
- Never run anything as root from the app.
- Anything on a system disk must be refused in the backend, not only greyed out in the window.
- Destructive dialogs keep Cancel as the default button.
