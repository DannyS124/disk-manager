# DiskForge

A disk manager for Linux that looks and works like Windows Disk Management. I've been on Arch for
a while and wanted something simple for managing drives without going to the terminal, so I made this.

![DiskForge](docs/screenshot.png)

## What it does
- Mount and unmount drives
- Format (ext4, btrfs, NTFS, FAT32, exFAT)
- Create, delete and resize partitions
- Rename volumes
- Make a new partition table (GPT or MBR)

It doesn't run as root. Changes go through udisks2, so you get the normal password prompt, and the
drive your system is on is locked so you can't format it by accident.

## Install (Arch)
This installs the latest release. If there's no release on the
[Releases page](https://github.com/DannyS124/diskforge/releases) right now, build from source instead (see Building).
```
git clone https://github.com/DannyS124/diskforge.git
cd diskforge/packaging/arch
makepkg -si
```
The PKGBUILD checks the sha256 of the release tarball and won't build if it doesn't match.

For exFAT you need `exfatprogs`, for NTFS `ntfs-3g`, for XFS `xfsprogs`.

## Updating
Help → Check for Updates tells you if there's a new version. To update you don't need to
uninstall anything:
```
cd diskforge && git pull
cd packaging/arch && makepkg -si
```

## Building
```
cmake -S . -B build
cmake --build build
./build/diskforge
```
`sudo ./build/diskforge-selftest` runs every operation on a throwaway disk image, so you can test
changes without touching a real drive.

## Bugs / ideas
Open an issue. If it's about a specific disk, paste the output of `diskforge --dump`.

## License
GPL-3.0-or-later (0.3.0 and 0.4.0 were MIT).
