# Security

## Reporting a problem
If you find a way DiskForge could damage data it shouldn't touch, or any other security issue,
please report it privately through **Security → Report a vulnerability** on
https://github.com/DannyS124/diskforge instead of opening a public issue.

## Getting a genuine copy
The only official source is **https://github.com/DannyS124/diskforge**. Copies posted anywhere
else, including other GitHub accounts, Discord uploads or file-sharing sites, aren't from this
project and may have been modified.

- **Releases are hash-checked.** Every release has a `SHA256SUMS` file, and the PKGBUILD pins the
  release tarball's SHA-256, so `makepkg` refuses to build a file that was changed in any way.
  To check a download by hand: `sha256sum -c SHA256SUMS`.
- **Releases are signed.** Release tags and commits are signed with the maintainer's key, and
  GitHub shows them as **Verified**.
- **Read before you run.** The source is short and readable. Everything that changes a disk is in
  `src/udisks.cpp` and goes through UDisks2, so it asks for your password before any change.
