# DiskForge Rescue: the first screen opens the desktop, unless the boot menu asked for text mode.
[ -f ~/.bashrc ] && . ~/.bashrc
if [ "$(tty)" = /dev/tty1 ] && [ -z "${DISPLAY:-}" ]; then
    if grep -qw diskforge.text /proc/cmdline; then
        diskforge-rescue-welcome
    else
        diskforge-rescue-session
    fi
fi
