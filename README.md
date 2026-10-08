# Disk Manager

A Windows-style disk manager for Linux: a volume list on top, and each disk drawn as a bar of
partitions underneath. Built with C++ and Qt6 on UDisks2.

![Disk Manager](docs/screenshot.png)

It can mount and unmount, open a drive in the file manager, format, create, delete and resize partitions,
change labels, and write a new partition table (GPT or MBR).

## How it stays safe
- **No root GUI.** The app runs as your normal user and never opens block devices itself. It asks
  **UDisks2**, the system disk service Dolphin also uses. UDisks2 checks each change through polkit
  (KDE's password prompt).
- **System disks are locked.** A disk holding a mount outside `/run/media`, `/media` or `/mnt`, or an
  active swap partition, is shown with a lock. Every change to it is refused twice: once in the
  window, and again in the backend before anything reaches UDisks2.
- **Confirmations say what will be lost.** The dialogs list the volumes that will be erased, Cancel is
  always the default button, formatting asks twice, and a new partition table means typing the disk's name.
- **Ventoy sticks get a warning** before any change that would stop them from booting.

## Install (Arch)
```bash
cd packaging/arch
makepkg -si          # builds and installs the "disk-manager" package
```
Then open **Disk Manager** from the app menu. To uninstall, run `sudo pacman -R disk-manager`.

Optional format tools: `exfatprogs` (exFAT), `dosfstools` (FAT32), `ntfs-3g` (NTFS),
`btrfs-progs`, `xfsprogs`. Formats whose tool is missing are greyed out in the dialogs, with the
package name.

## Development
```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
./build/disk-manager --dump                 # print the disk layout as text
sudo ./build/disk-manager-selftest          # every operation, resizing included, on a throwaway 512 MB image
./build/disk-manager-selftest --guard       # the system disk is refused (run as yourself)
QT_QPA_PLATFORM=offscreen ./build/disk-manager-preview out/   # render the dialogs to PNGs
```
The self-test attaches an image file as a loop device and only ever touches that device. It runs as
root because there is nobody to type a polkit password.

## Roadmap
1. ✅ View disks, partitions, free space and usage
2. ✅ Mount, unmount and open in the file manager
3. ✅ Format, create and delete partitions, labels, new partition tables
4. ✅ Resize: shrink and grow (ext4, Btrfs, NTFS, FAT32; XFS grow with xfsprogs)
