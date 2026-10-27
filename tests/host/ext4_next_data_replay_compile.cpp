#include <ok/fs/ext4/txn.hpp>
using namespace ok;
extern "C" Status qualify_ext4_data_allocation_completion(
    driver::BlockDevice &device, fs::Ext4RecoveryBounds bounds)
{
    fs::Ext4RecoveryCompletion<4, 2> completion(
        false, true, fs::Ext4ReplayPolicy::bounded_data_allocation);
    return completion.run(device, bounds);
}
