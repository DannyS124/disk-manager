# DiskForge

A disk manager for Linux that looks and works like Windows Disk Management. I've been on Arch for a while and
wanted something simple for managing drives without going to the terminal, so I made this. It also comes as
DiskForge Live, a rescue USB stick that can start any PC and fix its drives.

![DiskForge](docs/screenshot.png)

## What you can do with it

**Everyday drive stuff.** Mount, unmount and safely remove drives. Format them (ext4, btrfs, NTFS, FAT32, exFAT),
with encryption if you want it. Create, delete, resize and rename partitions, set their type and flags, make a new partition table (GPT or
MBR), make a drive mount at startup, and open .iso and .img files like a drive.

**Keeping drives healthy.** Disk Health warns you before a drive dies and says why in plain words. You can scan for
bad sectors (with a map of the drive) and repair them, check for firmware updates, run Btrfs scrubs, see your Btrfs
snapshots, TRIM your SSDs and benchmark a drive.

**When something's gone wrong.** Find Lost Files gets deleted files back, or files off a drive that was formatted or
won't open, with previews before you save them. Recover Partitions puts back partitions that disappeared. Rescue
Copy gets what it can off a dying drive. Check and Repair fixes file system errors, and Inspect Partition Table shows
the partition table as it really is on the disk.

**USB sticks.** Write an ISO or disk image to a stick (compressed ones too, and Ubuntu/Debian live sticks that keep
your changes). Make a Windows 10 or 11 install stick with the Windows 11 options (no TPM check, no Microsoft
account). Check a stick for bad spots and for being fake. Make a DiskForge Live stick.

**Backups and cleanup.** Back up and restore drives or partitions, clone a drive onto another one, see what's using
your space, clean out old packages, logs and caches, and wipe or Secure Erase a drive before you sell it. Wipes and
checks have a Stop button.

**Extras.** Add-ons for your own actions, with a list to install them from and an Add-on Maker so you don't have to
write JSON (see [docs/ADDONS.md](docs/ADDONS.md)). Also themes, RAID and LVM, and power settings for hard drives.

It doesn't run as root. Changes go through udisks2, so you get the normal password prompt, and the drive your
system runs from is locked so you can't format it by accident.

## Getting it

### Arch
This installs the latest release:
```
git clone https://github.com/DannyS124/diskforge.git
cd diskforge/packaging/arch
makepkg -si
```
The PKGBUILD checks the release's sha256 and won't build if it doesn't match.

### Other distros
Every release has an AppImage and a Flatpak on the [Releases page](https://github.com/DannyS124/diskforge/releases).
For Debian and Ubuntu, `packaging/deb/build.sh` builds a .deb (it needs podman and builds in a Debian container).
They're not on Flathub, because a disk manager needs more access than Flathub allows. In the Flatpak, Disk Usage
only sees your home folder and mounted drives, and add-ons need one extra permission (see Help → Problems).

### Extra packages for some features
DiskForge never installs anything by itself. If a feature needs something, it tells you which package. These are
the ones it can use (Arch names):

| For | Install |
|---|---|
| exFAT, NTFS, XFS | `exfatprogs`, `ntfs-3g`, `xfsprogs` |
| Btrfs scrubs | `btrfs-progs` |
| Disk Cleanup (package cache), snapshot list | `pacman-contrib`, `snapper` |
| Firmware check | `fwupd` |
| LVM | `udisks2-lvm2` |
| Make a Windows USB (most Windows 11 ISOs) | `wimlib` |
| Look-only add-ons | `bubblewrap` |

## DiskForge Live

![DiskForge Live's home screen](docs/diskforge-live.png)

DiskForge Live is a small Linux system on a USB stick, with DiskForge and a few other repair tools on it. It starts
any PC, new or old, even one whose own system won't start, and it opens on the screen above. To make one:

1. Download `diskforge-live-<version>.iso` from the [Releases page](https://github.com/DannyS124/diskforge/releases).
2. In DiskForge, go to File → Make a DiskForge Live USB, pick the ISO and the stick, and type the stick's name to
   confirm.
3. Plug the stick into the PC you want to fix, turn it on and press its boot menu key (usually F12, F11, F9 or Esc),
   then pick the stick.

Made this way, the stick still works for files and keeps logs of every start. Rufus, balenaEtcher or dd work too,
but then the stick is read-only. More in [docs/DISKFORGE-LIVE.md](docs/DISKFORGE-LIVE.md).

## How do I...

**Get deleted files back?** Action → Find Lost Files, pick the drive, tick the files you want and save them to a
*different* drive. If the drive is failing, make a Rescue Copy first and look in the copy.

**Know if a drive is dying?** Right-click it → Disk Health. A bar at the top also warns you when one starts going.

**Fix a USB stick that still shows old partitions from an ISO?** Right-click it → Wipe Disk, then New Partition Table
and one new partition.

**Erase a drive for good?** Wipe Disk is enough for a hard drive. For an SSD use Secure Erase, which has its own guide
with pictures: [docs/SECURE-ERASE.md](docs/SECURE-ERASE.md).

**Save what's left on a dying drive?** Rescue Copy it to an image file or another drive, then open the copy and work
on that, never the dying drive.

The built-in Help (F1) covers everything else, step by step.

## When something goes wrong

**Every change fails with "Not authorized".** Your desktop isn't running a polkit agent. KDE and GNOME have one, but
on tiling window managers you have to start one yourself (polkit-kde-agent, polkit-gnome or hyprpolkitagent).

**It says something "needs" a package.** Install that package (see the table above) and try again.

**A drive is missing.** Press F5. Empty card readers are hidden on purpose.

**Something else.** Open an issue. If it's about a specific drive, paste the output of `diskforge --dump`. If it went
wrong while DiskForge was doing something, run `diskforge --log ~/diskforge.log`, do it again and attach the log. It
lists what DiskForge asked the system to do and what came back, never passwords.

## Updating
Help → Check for Updates tells you when there's a new version. You don't need to uninstall anything first:
```
cd diskforge && git pull
cd packaging/arch && makepkg -si
```

## Building
You need CMake, Qt 6.8 or newer (base, svg and tools), udisks2, OpenSSL, zstd, libarchive and libblkid. On Arch:
`pacman -S cmake qt6-base qt6-svg qt6-tools udisks2 openssl zstd libarchive util-linux-libs`.
```
cmake -S . -B build
cmake --build build
./build/diskforge
```
`sudo ./build/diskforge-selftest` runs every operation on a throwaway disk image, so you can test changes without
touching a real drive. `packaging/release.sh` lists the other test suites.

## License
GPL-3.0-or-later (0.3.0 and 0.4.0 were MIT).
