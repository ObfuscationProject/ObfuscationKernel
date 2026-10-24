#include "ok/abi/linux_exec.hpp"

using Bootstrap = ok::abi::linux_user::ExecBootstrap<ok::abi::linux_user::Profile::OK_EXEC_TEST_PROFILE>;

ok::Result<ok::abi::linux_user::ExecImage> inspect_linux_exec(std::span<const std::byte> prefix, ok::usize file_bytes)
{
    return Bootstrap::inspect(prefix, file_bytes);
}

ok::Result<ok::usize> encode_linux_auxv(std::span<ok::u8> destination,
                                      const ok::abi::linux_user::ExecMetadata &metadata,
                                      const ok::abi::linux_user::AuxvParameters &parameters)
{
    return Bootstrap::encode_auxv(destination, metadata, parameters);
}
