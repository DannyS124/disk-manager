# DiskForge Live: the first screen opens the desktop, unless the boot menu asked for text mode.
[ -f ~/.bashrc ] && . ~/.bashrc
if [ "$(tty)" = /dev/tty1 ] && [ -z "${DISPLAY:-}" ]; then
    if grep -qw diskforge-live.text /proc/cmdline; then
        diskforge-live-welcome
    else
        diskforge-live-session
    fi
fi
