# Native and Linux PID 1 adoption

The exported SDK advertises
`native_init_profile=designated-pid1-native-linux-adoption-v2`. ABI67 alone does
not identify this behavior. The OS consumer requires that exact capability;
the prior `designated-pid1-adoption-v1` export is rejected by the current consumer.

When a published user process exits, its user children move to the live,
published designated user PID 1. Native and Linux personalities participate.
This includes already-completed children, which remain waitable. Live children
also acquire the adopter's signal-owner epoch and named thread identity before
another user task can run. A live child subsequently observes `getppid()==1`,
and its later exit uses the new parent lineage. Native PID 1 retains its polling
wait contract and receives no Linux SIGCHLD.

The ordinary exit, deferred native TERM/KILL and checked kernel administrative
termination paths use the same adoption transaction. Administrative native
termination preserves immediate reaping of the terminated parent, after its
children have been rebound. Linux administrative termination remains waitable.
Unpublished processes cannot select an adopter. Childless exit does not require
adoption signal-owner state, preserving partial fork rollback. Without a live
designated init, existing orphan/reaper behavior remains in effect.

This mechanism is serialized on the boot CPU. It does not synthesize Linux
SIGCHLD or apply an adopting Linux parent's SIG_IGN/SA_NOCLDWAIT policy to an
already-zombie orphan. Full Linux orphan notification, subreapers, process-group
job control, native signal handlers, synchronized shutdown and SMP remain open.

Hosted ordinary production tests execute the actual runtime and dispatcher with
mock trap/copy/mapping backends. The fixed Debug and Release suites each pass
398 checks under ASan/UBSan. The existing lifecycle suite passes 1,459 checks for
each of ten hosted architecture selections, and original administrative
regressions pass 303 checks in each mode. Hosted selections are not ten native
guests.

The SDK-only OS `--native-adoption --shell-acceptance` service-graph fixture
passes actual x86_64 Debug and Release QEMU execution. A native parent deliberately
leaves one live and one completed child; the former observes its new parent,
and PID 1 reaps exit values 23 and 31. The completed child retains the only pipe
writer until synchronous exit cleanup; EOF orders that exit before the parent
departs. Stricter independent readers validate marker causality and raw wait
statuses. These runs also cover termination, dependency restart and shell
pipelines. They end through host retirement after shell `sync`; their successful
read-only fsck checks do not establish a synchronized shutdown or power-loss
durability.

Evidence is retained under the workspace `build/qualification/`:

- `adoption-admin-fix-production-{debug,release}-v1/result.json`
- `adoption-admin-fix-lifecycle-all-v1/result.json`
- `adoption-admin-fix-original-admin-regression-v1/result.json`
- `native-adoption-sdk-v3/result.json`
- `native-m7-adoption-debug-v2/result.json`
- `native-m7-adoption-release-v1/result.json`
- `native-m7-adoption-independent-v1/{debug,release}/result.json`

Earlier SDK snapshots and the first failed adoption observer remain separate
records. The first observer timed out because it confused raw wait statuses
with exit values and missed a marker appended to a partial shell banner; it
has not been relabeled as a passing run. Complete M7 and M0-M12 remain open.
