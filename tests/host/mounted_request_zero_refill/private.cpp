#include <ok/fs/ext4_mounted_metadata.hpp>
using Mount = ok::fs::Ext4MountedMetadata<2>;
#if defined(OK_PRIVATE_STORAGE)
using Denied = Mount::TypedMutationStorage;
#elif defined(OK_PRIVATE_ZERO)
auto denied = &Mount::typed_private_zero;
#elif defined(OK_PRIVATE_REFILL)
auto denied = &Mount::typed_private_refill;
#elif defined(OK_PRIVATE_CLASSIFIER)
auto denied = &Mount::typed_classify_request;
#else
#error "Select one private access control"
#endif
