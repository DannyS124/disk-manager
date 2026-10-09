# Making a DiskForge add-on

An add-on is one file, `addon.json`, that adds actions to DiskForge's right-click menu and Tools
menu. An action runs a command on the selected drive.

Put it in `~/.local/share/diskforge/addons/<id>/addon.json`, or use Tools → Add-ons → Install from
File. There are examples in [`examples/addons`](../examples/addons).

## Example
```json
{
  "id": "open-terminal",
  "name": "Open Terminal Here",
  "version": "1.0",
  "author": "you",
  "description": "Opens a terminal in a mounted drive's folder.",
  "actions": [
    {
      "label": "Open Terminal Here",
      "icon": "utilities-terminal",
      "applies_to": "volume",
      "when": ["mounted"],
      "command": ["konsole", "--workdir", "{mountpoint}"]
    }
  ]
}
```

## Fields
- `id`: lowercase letters, digits and dashes. Also the folder name.
- `name`, `version`, `author`, `description`: shown in the Add-ons window.
- `actions`: one or more of:
  - `label`: the menu text.
  - `icon`: an icon name from your icon theme (optional).
  - `applies_to`: `volume` (a partition), `disk`, `free` (unallocated space) or `any`.
  - `when`: conditions that all have to be true: `mounted`, `unmounted`, `removable`, `internal`,
    `encrypted`, `unlocked`, `locked`, `has-health`, or `filesystem:ext4|vfat` (any of those).
  - `command`: the program and its arguments, as a list. Placeholders: `{device}` (`/dev/sdb1`),
    `{disk}` (`/dev/sdb`), `{mountpoint}`, `{label}`, `{uuid}`, `{fstype}`, `{size}` (bytes),
    `{model}`, `{home}`.
  - `terminal`: `true` to run it in a terminal window so you can see the output.
  - `confirm`: a question to ask before running (placeholders work here too).
  - `look_only`: `true` for actions that only look at things. They run in a read-only sandbox
    (needs bubblewrap): no changing files, no network, and other programs' sockets aren't there.
    A shell is fine in there.
  - `system_disks`: `true` to also offer a `look_only` action on the disk your system runs from.
    Without `look_only` it's ignored.

## Rules
- Commands run as you, never as root, and without a shell. If something needs root, put `pkexec` in
  the command (or `sudo` with `"terminal": true`). DiskForge then warns and asks before every run.
- Whoever made a drive picked its name, so values from the drive (`{label}`, `{model}`, `{uuid}`,
  `{fstype}`, `{mountpoint}`) are checked first:
  - one that would start an argument with `-` isn't run, so a drive named `--delete` can't become
    an option. If a value goes first in an argument to a program that takes options, put `--`
    before it anyway;
  - `/` in a name becomes `_`, so a name stays one folder name (UDisks does the same under
    /run/media);
  - names that are `.` or `..`, or have hidden characters in them, are refused.
- The program to run (the first part of `command`) can't come from the drive: only `{home}` works
  there.
- Before an action runs, DiskForge shows the command, one part per line, and asks. "Don't ask again"
  is remembered for that exact add-on file, so any change to the file asks again. Actions that use
  admin power, or run a shell or script interpreter, ask every time.
- Only `look_only` actions with `system_disks` are offered for the disk your system runs from.
- DiskForge notes the add-ons it installs. One that turns up in the folder some other way (or
  changes) is marked "Added outside", and its actions warn about it until you press I Added It in
  Tools → Add-ons. Hand-made add-ons get that too: press I Added It once you've checked yours.
- Only install add-ons you trust, the same as any other program.

## Getting it into the list
The list in Tools → Add-ons → Get Add-ons comes from
[github.com/DannyS124/diskforge-addons](https://github.com/DannyS124/diskforge-addons). Open a pull request
there with your `addons/<id>/addon.json`. Once it's merged it goes into the list.

DiskForge only installs from that repository, only files pinned to a commit, and only if the file matches
the checksum in the list. The list itself is signed with the maintainer's key, which isn't stored on GitHub,
and DiskForge only uses it if the signature checks out. It still shows the commands and asks before
installing, and again before each action first runs. Shells and script interpreters are only accepted in
`look_only` actions there.
