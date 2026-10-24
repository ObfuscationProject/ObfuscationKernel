#include <ok/fs/ext4_completion.hpp>
using namespace ok;
extern "C" Status qualify_ext4_completion(driver::BlockDevice &device, fs::Ext4RecoveryBounds bounds)
{
    fs::Ext4RecoveryCompletion<> completion;
    return completion.run(device,bounds);
}
