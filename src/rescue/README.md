# Kernel rescue diagnostics

`src/rescue/` holds the kernel-resident programs: the debug shell (`oksh`
diagnostic variant), the file manager and the task manager. They are scheduler-
visible kernel programs used for rescue and diagnostics; they are not product
applications (ObfuscationOS `docs/decisions/0002`). Their public headers stay under
`include/ok/apps/` until the OS desktop replaces the last caller, after which the
headers move here too. They compile in every build today because the debug test
points exercise the shell; gating them behind `kernel_gui` is the next step of
`refactor/user-runtime-split` once the test points stop depending on them.
