# Cross-parent regular move integration model

The composed source adds distinct journal, transaction, strict replay, cold
binding and completion types on the qualified planner/witness parent. All
previous source bytes, directory modes and completed commands remain retained.

The new hosted model consumes the existing 32 MiB Linux-created image after
the qualified mkdir/populated-directory rename composition. It moves the
4 KiB inode 12 from root `seed` to directory 14 `moved.txt`, preserving its
generation, allocation, link count and all file data. It covers every whole
old/new home combination, publication and completion write/FLUSH failures,
partial writes, unknown witnesses, corrupt records and snapshot drift.

After completion it separately mounts the empty-head state and renames the
populated parent to `files-archive`, checking retained Node/name identities
and content. This is a transport/device model and a mounted directory rename;
the file move itself is not a mounted VFS operation or native application.
All model power losses are synthetic and do not assert hardware durability.

No original model, compiler, archive, fixture or metadata command is repeated.
The worker must compile and link one new model, execute it once, then run two
read-only e2fsck commands against its new output images. Independent source and
exact worker review precede execution. Failures retain all partial success and
require a missing-only successor. The default SDK remains unchanged. Mounted
move ownership, startup classification, native qualification and Files28
execution are still separate required gates; full M0-M12 remains incomplete.
