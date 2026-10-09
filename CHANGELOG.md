# Changelog

## 0.5.0
Everything new since 0.4.3.

Safety:
- A drive's name can't steer an add-on anymore: a name starting with "-" can't turn into an option, "/" and ".." can't point at another folder, and names with hidden characters are refused
- Add-ons that use admin power (pkexec, sudo) or run a shell ask every time, with a warning. The question shows the command one part per line
- Look-only add-ons (new `look_only` field) run in a read-only sandbox: they can't change files, use the network or reach other programs. Only these are offered on the system disk
- "Don't ask again" is tied to the add-on file, so any change to the file asks again
- Add-ons that turn up in the add-on folder without going through DiskForge are flagged until you say they're yours
- The online add-on list is signed with a key that isn't on GitHub, and DiskForge checks the signature
- Drive names, file names and backup descriptions are shown as plain text, and hidden characters (like ones that flip text direction) are dropped
- Check for Updates builds the release link itself instead of opening whatever link comes back
- Rescue maps whose blocks don't add up are refused, and Clone refuses partition tables that run past the end of the drive
- Every release now runs crash tests: thousands of damaged add-ons, lists, signatures, backups, maps and partition tables, also in a build with memory checks

New:
- Clone Drive: copies a whole drive onto another one, skips empty space, and can grow the last partition into a bigger drive. Pick whether the copy replaces the old drive (same IDs) or sits next to it (new IDs)
- Back Up and Restore: saves a drive or partition to a compressed .img.zst file with a checksum, and checks the whole backup before restoring anything
- Rescue Copy: gets what's still readable off a failing drive, easy parts first. Stop it any time and carry on later; the map file works with GNU ddrescue too
- Scan for Bad Sectors now draws a map of the drive as it goes: green is fine, orange is slow, red can't be read
- Disk Usage: a map of what's using the space on a drive, click into folders
- Disk Cleanup: old package files, packages you've uninstalled, old system logs, your cache folder and the trash
- Optimize Drives: TRIM now, or every week
- Btrfs Snapshots: see your subvolumes and snapper's snapshots
- Secure Erase: the drive's own erase command (ATA and NVMe), with help for drives that are "frozen"
- Get Add-ons: install add-ons from the online list (checked against a checksum before installing)
- Flatpak and AppImage downloads on each release
- Ready for translations (see CONTRIBUTING.md)
- Disk health (SMART): healthy/warning/failing on every disk, temperature, hours, bad sectors, self-tests
- Write an ISO to a USB stick, with an optional SHA-256 check and verification afterwards
- Encrypted drives: encrypt when formatting or creating a partition, unlock, lock, change passphrase
- Wipe a whole disk with zeros
- Check for errors and repair
- Mount at startup (adds an /etc/fstab entry under /mnt)
- Safely remove USB drives
- Open .iso and .img files as disks
- Benchmark read speed, access time and write speed
- Progress bar for long operations
- Fix: actions that ask something first (repair after a check, format, delete, resize...) could fail with "disk not found" if the disk list refreshed while the question was open
- Right-click any partition, free space or drive to get everything you can do with it, partition and drive options together
- Scan for bad sectors and repair them (rewrites just those sectors so the drive swaps in spares)
- Long self-test button in Disk Health
- Add-ons: JSON files that add menu actions (Tools → Add-ons), with three examples in examples/addons

## 0.4.3
- Check for Updates says so when there's no release, instead of "Not Found"
- Includes the 0.4.2 wording changes (0.4.2 was taken down before it was tested)

## 0.4.2
- Simpler wording in the help, About window and docs

## 0.4.1
- Switched the license to GPL-3.0-or-later

## 0.4.0
- Renamed to DiskForge
- Help (F1), About window, Check for Updates
- App icon, man page, AppStream metadata

## 0.3.0
- Resize partitions (ext4, btrfs, NTFS, FAT32)

## 0.2.0
- Format, create and delete partitions, rename, new partition tables

## 0.1.0
- First version: view disks, mount/unmount
