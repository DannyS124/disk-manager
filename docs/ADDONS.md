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
  - `system_disks`: `true` to also offer it on the disk your system runs from. Only for tools that
    don't change anything.

## Rules
- Commands run as you, never as root, and without a shell, so a drive label can't sneak in extra
  commands. If something needs root, put `sudo` or `pkexec` in the command and you'll be asked.
- The first time an action runs, DiskForge shows the exact command and asks. If the add-on file
  changes, it asks again.
- Add-ons aren't offered for the system disk unless they set `system_disks`.
- Only install add-ons you trust, the same as any other program.

## Getting it into the list
The list in Tools → Add-ons → Get Add-ons comes from
[github.com/DannyS124/diskforge-addons](https://github.com/DannyS124/diskforge-addons). Open a pull request
there with your `addons/<id>/addon.json`. Once it's merged it goes into the list.

DiskForge only installs from that repository, only files pinned to a commit, and only if the file matches
the checksum in the list. It still shows the commands and asks before installing, and again before each
action first runs.
