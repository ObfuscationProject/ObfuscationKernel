# IPC

`ok::ipc::IpcRouter` creates bounded message channels. Messages carry sender and
receiver process IDs plus up to 128 bytes of inline payload.

The templated `send_value` API accepts trivially serializable payloads and is
constrained by `MessagePayload`.

