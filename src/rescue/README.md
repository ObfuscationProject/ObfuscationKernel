# Kernel rescue diagnostics

`src/rescue/` holds the kernel-resident programs: the debug shell (`oksh`
diagnostic variant), the file manager and the task manager. They are scheduler-
visible kernel programs used for rescue and diagnostics; they are not product
applications (ObfuscationOS `docs/decisions/0002`). Their public headers stay under
`include/ok/apps/` until the OS desktop replaces the last caller, after which the
headers move here too.

They are compiled with `kernel_gui` and in debug builds, whose test points drive
them. Other builds compile `disabled.cpp` instead (it is empty unless xmake
defines `OK_DISABLE_RESCUE_APPS`): the public interfaces stay linkable for
`Kernel`, its GUI paths and ObfuscationOS modules, and behave like applications
that were never opened. An SDK provides the applications when its manifest has
`kernel_gui` set or `mode` is `debug`.
