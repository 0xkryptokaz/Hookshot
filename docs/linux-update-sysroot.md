# Sysroot library aliases in Linux update parts

The AppImage's selected system libraries are copied by value into its sysroot.
Some host distributions make a development alias such as libmvec.so an absolute
symlink. Keeping that target would either depend on the player's host filesystem
or be rejected by the updater's absolute-target symlink guard. Dereferencing the
source alias while copying preserves its bytes and attributes inside the part.

The internal relative lib -> usr/lib sysroot layout remains a link. The ZIP writer
and updater retain their existing path/link checks; absolute-target ZIP links are
still rejected. Existing parts carrying such links are not retroactively rewritten.
Tests execute the production copy loop with private library inputs, then package
and unpack the resulting sysroot, alongside the hostile-archive regression cases.
