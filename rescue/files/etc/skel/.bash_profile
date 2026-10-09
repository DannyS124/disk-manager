# Bluespark: the first screen opens the desktop, unless the boot menu asked for text mode.
[ -f ~/.bashrc ] && . ~/.bashrc
if [ "$(tty)" = /dev/tty1 ] && [ -z "${DISPLAY:-}" ]; then
    if grep -qw bluespark.text /proc/cmdline; then
        bluespark-welcome
    else
        bluespark-session
    fi
fi
