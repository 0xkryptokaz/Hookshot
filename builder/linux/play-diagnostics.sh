# Run from the game folder. The Builder supplies this as /bin/sh -c source so
# it is available even after the Builder's AppImage mount goes away.
umask 077
temporary=$(mktemp -d .hookshot-startup.XXXXXX) || exec "$1"
trap 'rm -f -- "$temporary/stderr" "$temporary/status"; rmdir -- "$temporary"' EXIT

# The game keeps stdout discarded. Drain all of stderr so a noisy game can
# continue after the retained prefix is full.
{
    "$1" 2>&1 >/dev/null
    printf '\nHookshot exited with status %s\n' "$?" > "$temporary/status"
} | {
    head -c 65536 > "$temporary/stderr"
    cat >/dev/null
}
if [ -f "$temporary/stderr" ] && [ -f "$temporary/status" ]; then
    cat "$temporary/status" >> "$temporary/stderr" &&
        mv -fT -- "$temporary/stderr" hookshot_startup.log
fi
