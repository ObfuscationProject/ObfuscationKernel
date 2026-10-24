// Included inside the serialized native runtime's private namespace.
template <posix::ReadinessPolicy Policy>
Result<posix::DescriptorReadiness> retained_description_readiness(
    Kernel &kernel, u16 id, posix::ReadyEvents requested)
{
    const auto *description = kernel.scheduler().open_description(id);
    if (!description || !description->references)
        return posix::DescriptorReadiness{.events = posix::ReadyEvents::invalid};
    if (!description->native_udp_handle)
        return kernel.posix().poll_description<Policy>(id, requested);
    const auto joined = kernel.native_udp_executor().readiness(
        kernel.native_udp(), description->native_udp_handle);
    return net::native_udp_descriptor_readiness(joined, description->readable,
                                               description->writable, requested);
}
