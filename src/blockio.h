// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Low-level helpers for reading and writing devices through fds that UDisks2 opened.

#include <QString>

#include <cstdlib>
#include <memory>

namespace blockio {

constexpr size_t kAlign = 4096; // O_DIRECT wants aligned buffers, offsets and sizes

using Buffer = std::unique_ptr<char, decltype(&free)>;
Buffer alignedBuffer(size_t size);

// Whole-length reads and writes; false on any error or short transfer.
bool readAt(int fd, char *buf, quint64 len, quint64 offset);
bool writeAt(int fd, const char *buf, quint64 len, quint64 offset);
// Sequential, at the fd's current position.
bool writeAll(int fd, const char *data, qint64 size);
qint64 readFull(int fd, char *data, qint64 size);

int logicalSize(int fd);
int physicalSize(int fd);
quint64 deviceSize(int fd); // block device or regular file

// Whether `path` lives on `diskDevice` (/dev/sdb), following partitions, LUKS/LVM
// mappings and Btrfs subvolumes. Used to refuse writing a copy onto its own source.
bool pathIsOnDisk(const QString &path, const QString &diskDevice);

} // namespace blockio
