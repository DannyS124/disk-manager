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
- Programs DiskForge starts (add-ons, terminals) don't get handed an open drive anymore

New:
- Stop button: wipes, checks and self-tests get a bar at the top showing how far along they are, with a Stop button (also on the toolbar). A drive's own Secure Erase says up front that it can't be stopped
- A warning bar at the top when a drive is failing or needs a look, with Back Up (or Rescue Copy), Details and Dismiss. It comes back if things get worse
- Disk Health explains itself: every reason behind the verdict, what each number means, and the ones that count are in bold. It goes by the numbers Backblaze's drive stats tie to failures. Connection errors say to check the cable, since the drive itself may be fine
- Firmware: Disk Health shows the drive's firmware and asks fwupd if there's a newer one. DiskForge never installs firmware itself
- Btrfs: error counts for each Btrfs partition in Disk Health, and a scrub now or every month
- Recover Partitions: put a lost or damaged partition table back, from the copy GPT keeps at the end of the drive, from a layout DiskForge saw on that drive before (it keeps the last 10), or by scanning the drive for file systems
- Inspect Partition Table: the MBR, both GPT copies and the partition list as they are on the drive, checksums checked, plus any sector in hex. Only reads, so it works on the system disk too
- Partition type and flags (GPT attributes, the MBR boot flag)
- A lock on encrypted partitions in the map: click it to unlock or lock
- Power settings for hard drives: when to spin down, how hard to save power, the write cache, and Sleep Now
- Rescue Copy can go easy on a failing drive: pause when it's too hot, rest after a run of read errors, or cap the speed
- RAID arrays are listed like drives, with their status (checking, rebuilding, degraded), and you can start a check
- LVM volume groups are listed like drives, their logical volumes as the volumes (needs udisks2-lvm2)
- Add-ons can ask for things in a form, have their own settings, and show their output in a window. Every add-on action is in the Tools menu, with the reason when one doesn't fit, and you can pin actions to the toolbar or give them a shortcut
- Add-on Maker: make or change an add-on in a window instead of writing JSON
- Themes: Classic, Deadshadow and High Contrast built in, theme add-ons, and a Theme Maker. Danger stays red and text stays readable whatever the theme
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
