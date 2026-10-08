# Contributing

PRs are welcome. If you change anything in `src/udisks.cpp`, run the self-test first:
```
sudo ./build/diskforge-selftest
./build/diskforge-selftest --guard
```
Don't open block devices directly or run anything as root. Everything goes through udisks2.

Want to add something without touching the code? Make an add-on instead: see `docs/ADDONS.md`.

Contributions are GPL-3.0-or-later like the rest of the project.
