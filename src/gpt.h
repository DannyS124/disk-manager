// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// GPT keeps a backup of its header and partition list in the disk's last sectors. After
// copying a disk onto a bigger one, that backup sits in the middle of the new disk;
// relocateBackup moves it to the real end and lets the partition list grow into the
// new space. Works on the fd a clone or restore already holds, so no second prompt.

#include <QString>

namespace gpt {

struct Result {
    bool ok = false;
    QString error;
};

// sectorSize 0 = ask the device (regular files report 512).
bool isGpt(int fd, int sectorSize = 0);
// newGuids: give the disk and its partitions new unique IDs, so a clone kept next to
// its original doesn't share them.
Result relocateBackup(int fd, int sectorSize = 0, bool newGuids = false);

} // namespace gpt
