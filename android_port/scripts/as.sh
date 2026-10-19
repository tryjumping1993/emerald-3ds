#!/bin/sh
# GNU as for the game's data assembly, then its .rodata made writable. The data holds
# pointers (script and map tables, song headers) that a shared library must relocate, and
# the loader may relocate only writable sections: they all end up in .gamedata, which is
# NOLOAD and filled from the data pack at start-up anyway (emerald3ds_android.ld.in).
out=
prev=
for arg in "$@"; do
	[ "$prev" = "-o" ] && out=$arg
	prev=$arg
done
"$CTR_AS" "$@" || exit $?
"$CTR_OBJCOPY" --set-section-flags .rodata=alloc,load,data --set-section-flags .rodata.ctr_script_keep=alloc,load,data "$out"
