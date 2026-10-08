# DiskForge

A disk manager for Linux that looks and works like Windows Disk Management. I've been on Arch for
a while and wanted something simple for managing drives without going to the terminal, so I made this.

![DiskForge](docs/screenshot.png)

## What it does
- Mount, unmount and safely remove drives
- Format (ext4, btrfs, NTFS, FAT32, exFAT), with optional encryption
- Create, delete and resize partitions, rename volumes
- Make a new partition table (GPT or MBR), or wipe a whole disk
- Check for errors and repair
- Make a drive mount at startup
- Disk health: warns you before a drive dies, scan for bad sectors (with a map of the drive) and repair them
- Clone a drive onto another one, back up and restore drives or partitions (compressed, with checksums)
- Rescue Copy: get what you can off a dying drive, like ddrescue
- Disk Usage: see what's eating your space
- Disk Cleanup: old packages, logs, caches and the trash
- Optimize Drives (TRIM), Btrfs snapshot list, Secure Erase for SSDs
- Write an ISO to a USB stick (Arch install sticks etc.)
- Open .iso and .img files like a drive
- Benchmark drive speed
- Add-ons: anyone can add their own actions (see [docs/ADDONS.md](docs/ADDONS.md)), and there's a list to install them from

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

For exFAT you need `exfatprogs`, for NTFS `ntfs-3g`, for XFS `xfsprogs`. Disk Cleanup uses
`pacman-contrib` for the package cache, and the snapshot list needs `snapper`.

## Other distros
Each release also has a Flatpak and an AppImage on the Releases page. They're not on Flathub (a disk
manager needs more access than Flathub allows). In the Flatpak, Disk Usage only sees your home folder and
mounted drives, and add-ons need one extra permission (see Help → Problems).

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
changes without touching a real drive. There are more test suites (clone, backup, rescue and so on);
`packaging/release.sh` lists them all.

## Bugs / ideas
Open an issue. If it's about a specific disk, paste the output of `diskforge --dump`.

## License
GPL-3.0-or-later (0.3.0 and 0.4.0 were MIT).
