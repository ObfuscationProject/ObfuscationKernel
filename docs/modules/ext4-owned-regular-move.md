# Distinct mounted regular-file move owner

This source slice composes the dedicated `OKMVW001` writer and strict completion
with the canonical ext4 owner. It retains absent-destination, different-parent,
singly linked ordinary regular files, two checked one-block directories,
unmodified allocation and an independently empty orphan chain. Existing
same-parent, directory-create, namespace and orphan recovery families retain
their own admission paths.

`prepare_regular_move` uses the owner's actual exclusive writable Session.
Every resident observed input is compared before any cache pin can evict it;
every participating pinned canonical preimage is then checked again. The full
journal head, both directories and coalesced inode-table homes are staged in one
cache transaction. Commit rechecks the writer's complete physical snapshots.
Checkpoint admits the dedicated move proof, compares its full journal, homes
and unchanged ancillary history to the original writer, executes completion and
publishes only physically verified after images. The original noncopyable
owner/cache/Session/transaction/slot/generation authority remains address-bound.
The public generic completion report receives only the completed journal
sequence for existing renewal observation; the distinct move completion report
carries move durability and physical-verification evidence.

Startup classifies the explicit move witness before the legacy route. A private
noncopyable service view dispatches every actual request through the same
canonical Session with an immutable original cursor, exact before/attempted/
returned counters, selected operation phase and selected write bytes. Public
commit/checkpoint and other family services cannot borrow this cold route. The
cursor reserves the complete six-counter family allowance and continues across
the strict ordinary decoder, including actual canonical cache misses. Terminal
physical/cache bookends and exact empty-head equality precede owner rearm and
ordinary root admission. There is no namespace fallback or new Session.

`ext4_owned_regular_move_tests.cpp` authors normal-access hosted cases for
reader quiescence, resident stale head/ancillary rejection before eviction,
post-publication ancillary drift, exact separate completion evidence, every
whole-home old/new cold mixture, continuing mounted root admission and transport
failure without publication. The test and xmake source are authored here;
Root separately controls compilation, models, existing fixtures, fsck, SDK
exports and guest execution. Source composition alone does not claim mounted
execution, hardware qualification, SDK promotion, default Files workflow
readiness or full M0–M12 completion. M11 hardware and manual M12 remain reserved.
