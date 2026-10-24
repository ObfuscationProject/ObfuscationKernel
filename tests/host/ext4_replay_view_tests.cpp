#include "ext4_view_test_support.hpp"

using namespace ok;
using namespace ok::fs;
using namespace fixture;
namespace
{
Status sequence(Ext4ReplayPreview<> &preview, Device &device, bool dirty, bool inspect_only = false)
{
    auto opened = preview.open(device, {device.origin, device.bytes.size() / 512});
    if (!opened || inspect_only) return opened;
    auto found = preview.lookup(2, "seed"); if (!found) return found.status();
    auto info = preview.inode(found.value()); if (!info) return info.status();
    if (info.value().mode != (dirty ? 0100600 : 0100644) || info.value().uid != (dirty ? 1000U : 0U) ||
        info.value().gid != (dirty ? 1001U : 0U) || info.value().size != 4096)
        return Status::fault("preview inode differs from independent oracle");
    std::array<u8,4096> bytes{};
    auto read = preview.read(found.value(), 0, bytes); if (!read) return read.status();
    if (read.value() != bytes.size()) return Status::fault("preview read size");
    for (usize index = 0; index < bytes.size(); ++index)
        if (bytes[index] != u8(index * 17 + 29)) return Status::fault("preview file payload differs");
    return {};
}
void rejected(const Bytes &bytes, const char *name, const char *reason, usize &cases)
{
    Device device(bytes); Ext4ReplayPreview<> preview;
    const auto result = sequence(preview, device, true, true);
    if (result || preview.ready() || preview.report().phase != Ext4PreviewPhase::failed ||
        result.message().find(reason) == std::string_view::npos)
    {
        std::fprintf(stderr, "case=%s code=%u reason=%.*s\n", name, u32(result.code()), int(result.message().size()), result.message().data());
        check(false, "invalid preview not rejected at expected boundary");
    }
    check(preview.open(device, {0, bytes.size()/512}).code() == StatusCode::already_exists, "failed preview reused");
    device.untouched(); ++cases;
}
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream file(argv[1], std::ios::binary); check(bool(file), "open image");
    const Bytes original((std::istreambuf_iterator<char>(file)), {});
    const bool dirty = std::string_view(argv[2]) == "dirty";
    usize cases = 0;
    Device base(original); Ext4ReplayPreview<> preview;
    status(sequence(preview, base, dirty), "reference preview");
    const auto successful_reads = base.reads;
    check(preview.ready() && preview.report().validated_actions == (dirty ? 2U : 0U), "preview validated action count");
    check(base.bytes == original, "reference preview changed source image"); base.untouched();
    check(preview.open(base, {0, original.size()/512}).code() == StatusCode::already_exists, "successful preview reused");
    auto missing = preview.lookup(2, "absent"); check(!missing && missing.status().code() == StatusCode::not_found && preview.ready(), "missing name retires valid view");
    for (bool writable : {false, true})
    {
        Device device(original); device.writable = writable; device.origin = (u64{1} << 34) + 13;
        Ext4ReplayPreview<> at_offset; status(sequence(at_offset, device, dirty), "u64 extent origin"); device.untouched(); ++cases;
    }
    for (usize point = 1; point <= successful_reads; ++point)
        for (bool geometry : {false, true})
        {
            Device device(original); if (geometry) device.change_after = point; else device.fail_read = point;
            Ext4ReplayPreview<> failure; const auto result = sequence(failure, device, dirty);
            check(!result && !failure.ready() && failure.report().phase == Ext4PreviewPhase::failed, "read/geometry failure published a preview");
            device.untouched(); ++cases;
        }
    for (const auto error : {Status::unsupported("device read unsupported"), Status::not_found("device read absent")})
    {
        Device device(original);device.fail_read=successful_reads;device.read_failure=error;
        Ext4ReplayPreview<> failure;const auto result=sequence(failure,device,dirty);
        check(!result&&!failure.ready()&&failure.report().phase==Ext4PreviewPhase::failed,"nonfatal inode status hid a reader failure");
        device.untouched();++cases;
    }
    const auto mutation = [&](const char *name, const char *reason, const auto &change) {
        auto bytes = original; change(bytes); rejected(bytes, name, reason, cases);
    };
    const auto root_inode = inode_offset(original, 2);
    const auto root_block = r32(original, root_inode + 60) * block_size(original);
    mutation("directory checksum", "directory tail or checksum", [&](auto &b) { b[root_block + 8] ^= 1; });
    mutation("directory tail type", "directory tail or checksum", [&](auto &b) {
        b[root_block + block_size(b) - 5] ^= 1; directory_crc(b, 2, root_block);
    });
    mutation("rechecksummed directory record", "directory entry bounds", [&](auto &b) {
        w16(b, root_block + 4, 0); directory_crc(b, 2, root_block);
    });
    mutation("rechecksummed directory name", "invalid directory name", [&](auto &b) {
        b[root_block + 8] = '/'; directory_crc(b, 2, root_block);
    });
    mutation("root dot-entry identity", "root dot-entry identity", [&](auto &b) {
        w32(b, root_block, 12); directory_crc(b, 2, root_block);
    });
    mutation("root directory checksum generation seed", "directory tail or checksum", [&](auto &b) {
        w32(b, root_inode + 100, r32(b, root_inode + 100) + 1); inode_crc(b, 2, root_inode);
    });
    mutation("root directory scan capacity", "directory scan capacity", [&](auto &b) {
        w32(b, root_inode + 4, block_size(b) * 1025); inode_crc(b, 2, root_inode);
    });
    mutation("root inode checksum", "checked inode checksum", [&](auto &b) { b[root_inode + 124] ^= 1; });
    mutation("root extent outside volume", "data extent bounds", [&](auto &b) {
        w32(b, root_inode + 60, r32(b, 1028)); inode_crc(b, 2, root_inode);
    });
    mutation("root extent aliases journal", "data aliases journal", [&](auto &b) {
        w32(b, root_inode + 60, u32(journal(b)/block_size(b))); inode_crc(b, 2, root_inode);
    });
    mutation("root extent aliases inode table", "aliases metadata", [&](auto &b) {
        w32(b, root_inode + 60, r32(b, descriptor(b,0)+8)); inode_crc(b, 2, root_inode);
    });
    mutation("root high physical extent", "extent encoding", [&](auto &b) {
        w16(b, root_inode + 58, 1); inode_crc(b, 2, root_inode);
    });
    mutation("root external extent tree", "requires inline extents", [&](auto &b) {
        w16(b, root_inode + 46, 1); inode_crc(b, 2, root_inode);
    });
    mutation("root directory hole", "directory hole", [&](auto &b) {
        w16(b, root_inode + 42, 0); w32(b, root_inode + 28, 0); inode_crc(b, 2, root_inode);
    });
    mutation("root sector accounting", "sector accounting", [&](auto &b) {
        w32(b, root_inode + 28, 0); inode_crc(b, 2, root_inode);
    });
    {
        Device device(original); Ext4ReplayPreview<> changed; status(changed.open(device,{0,original.size()/512}), "prepare post-admission corruption");
        device.bytes[root_block + 8] ^= 1;
        auto lookup = changed.lookup(2,"seed"); check(!lookup && !changed.ready(), "post-admission directory corruption passed");
        device.untouched(); ++cases;
    }
    if (dirty)
    {
        const usize j = journal(original), size = block_size(original), seed_inode = inode_offset(original, 12);
        const usize at = j + size * 3 + seed_inode % size;
        mutation("rechecksummed superblock geometry change", "differs from bounded binding", [&](auto &b) {
            w32(b,j+2*size+4,r32(b,1028)-1); super_crc(b,j+2*size); linux_payload_crc(b,false);
        });
        mutation("rechecksummed immutable inode size", "mapping, allocation or namespace", [&](auto &b) {
            w32(b,at+4,4095); inode_crc(b,12,at); linux_payload_crc(b,true);
        });
        mutation("rechecksummed inode link count", "mapping, allocation or namespace", [&](auto &b) {
            w16(b,at+26,2); inode_crc(b,12,at); linux_payload_crc(b,true);
        });
        mutation("valid JBD2 with invalid ext4 inode checksum", "checked inode checksum", [&](auto &b) {
            b[at+124]^=1; linux_payload_crc(b,true);
        });
        mutation("invalid ctime nanoseconds", "ctime nanoseconds", [&](auto &b) {
            w32(b,at+132,4000000000U); inode_crc(b,12,at); linux_payload_crc(b,true);
        });
        mutation("journal ownership inode changed", "ordinary regular inode", [&](auto &b) {
            const auto journal_inode = inode_offset(b,8), target = journal_inode/size;
            for(usize n=0;n<size;++n)b[j+size*3+n]=b[target*size+n];
            big32(b,j+size+44,u32(target)); const auto inode_at=j+size*3+journal_inode%size;
            w16(b,inode_at,0100644); inode_crc(b,8,inode_at); linux_payload_crc(b,true);
        });
        mutation("superblock filesystem identity changed", "filesystem UUID changed", [&](auto &b) {
            b[j+size*2+104]^=1; super_crc(b,j+size*2); linux_payload_crc(b,false);
        });
        mutation("superblock reserved field change", "exceeds bookkeeping policy", [&](auto &b) {
            b[j+size*2+300]^=1; super_crc(b,j+size*2); linux_payload_crc(b,false);
        });
        mutation("unsupported allocation transaction", "allocation or structural metadata", [&](auto &b) {
            const usize gd=descriptor(b,0), bitmap=r32(b,gd), logical=7000U;
            for(usize n=0;n<size;++n){b[j+size*2+n]=b[gd+n];b[j+size*3+n]=b[bitmap*size+n];}
            b[j+size*3+logical/8]^=u8(1U<<(logical%8));
            w16(b,j+size*2+24,u16(crc(seed(b),b,j+size*3,r32(b,1056)/8)));
            group_crc(b,0,j+size*2);big32(b,j+size+12,u32(gd/size));big32(b,j+size+44,bitmap);
            linux_payload_crc(b,false);linux_payload_crc(b,true);
        });
        const auto appended = [&](bool committed, bool revoke) {
            auto b = original;
            for (usize offset = j + size * 5; offset < j + size * 9; ++offset) b[offset] = 0;
            const usize record = j + size * 5, payload = j + size * 6;
            big32(b, record, 0xc03b3998); big32(b, record + 4, revoke ? 5 : 1); big32(b, record + 8, 3);
            if (revoke)
            {
                big32(b, j + 40, 17); big32(b, j + 252, 0);
                big32(b, j + 252, crc(~u32{0}, b, j, 1024));
                big32(b, record + 12, 20); big32(b, record + 16, 7);
            }
            else
            {
                for (usize offset = 0; offset < size; ++offset) b[payload + offset] = b[j + size * 3 + offset];
                const usize changed_inode = payload + seed_inode % size;
                w16(b, changed_inode, 0100604); w16(b, changed_inode + 2, 1002); inode_crc(b, 12, changed_inode);
                big32(b, record + 12, 7); big32(b, record + 16, 8);
                const std::array<u8,4> seq{0,0,0,3};
                big32(b, record + 24, crc(ext2fs_crc32c_le(seed(b), seq.data(), seq.size()), b, payload, size));
            }
            journal_record_crc(b, record, size - 4);
            if (committed)
            {
                const usize commit = j + size * (revoke ? 6 : 7);
                for (usize offset = 0; offset < size; ++offset) b[commit + offset] = b[j + size * 4 + offset];
                big32(b, commit + 8, 3); journal_record_crc(b, commit, 16);
            }
            return b;
        };
        for (const bool committed : {false, true})
        {
            auto image = appended(committed, false); Device device(image); Ext4ReplayPreview<> multiple;
            status(multiple.open(device,{0,image.size()/512}), "additional metadata transaction");
            auto value = multiple.inode(12);
            check(value && value.value().uid == (committed ? 1002U : 1000U) &&
                  multiple.report().planned_actions == (committed ? 3U : 2U) &&
                  multiple.report().incomplete_tail == !committed, "latest committed overlay or incomplete tail");
            device.untouched(); ++cases;
        }
        {
            auto image = appended(true, true); Device device(image); Ext4ReplayPreview<> revoked;
            status(revoked.open(device,{0,image.size()/512}), "revoked inode transaction");
            auto value = revoked.inode(12);
            check(value && value.value().uid == 0 && value.value().mode == 0100644 &&
                  revoked.report().revoked_actions == 1 && revoked.report().planned_actions == 1,
                  "revoked payload leaked into final view");
            device.untouched(); ++cases;
        }
        {
            auto image = original; const u32 sequence = 0xffffffffU;
            big32(image,j+24,sequence);big32(image,j+252,0);big32(image,j+252,crc(~u32{0},image,j,1024));
            big32(image,j+size+8,sequence);linux_payload_crc(image,false);linux_payload_crc(image,true);
            big32(image,j+size*4+8,sequence);journal_record_crc(image,j+size*4,16);
            Device device(image);Ext4ReplayPreview<> wrapped;status(wrapped.open(device,{0,image.size()/512}),"wrapped journal sequence");
            check(wrapped.report().next_sequence==0 && wrapped.report().committed_transactions==1,"sequence wrap lost");
            device.untouched();++cases;
        }
        mutation("torn home inode outside bounded preview policy", "checked inode checksum", [&](auto &b) {
            b[seed_inode+124]^=1;
        });
        mutation("corrupt committed journal record", "JBD2 record checksum", [&](auto &b) {
            b[j+size*4+16]^=1;
        });
        {
            Device device(original); Ext4ReplayPreview<1> too_small;
            check(!too_small.open(device,{0,original.size()/512}) && !too_small.ready(),"preview action capacity silently truncated");device.untouched();++cases;
        }
        {
            Device device(original);Ext4ReplayPreview<> changed;status(changed.open(device,{0,original.size()/512}),"prepare journal payload corruption");
            device.bytes[at]^=1;auto got=changed.inode(12);check(!got&&!changed.ready(),"journal payload not revalidated on final read");device.untouched();++cases;
        }
    }
    else
    {
        {
            auto b=original;const usize gd=descriptor(b,0),bitmap=r32(b,gd+4)*block_size(b);
            b[bitmap+11/8]&=u8(~(1U<<(11%8)));
            w16(b,gd+26,u16(crc(seed(b),b,bitmap,r32(b,1064)/8)));group_crc(b,0,gd);
            Device device(b);Ext4ReplayPreview<> dangling;
            status(dangling.open(device,{0,b.size()/512}),"prepare dangling directory target");
            auto unallocated=dangling.inode(12);
            check(!unallocated&&unallocated.status().code()==StatusCode::not_found&&dangling.ready(),"direct unallocated inode lookup");
            auto target=dangling.lookup(2,"seed");
            check(!target&&target.status().code()==StatusCode::io_error&&!dangling.ready(),"dangling directory entry was treated as absent");
            device.untouched();++cases;
        }
        for(const auto name: {"short","long","hole","unwritten"})
        {
            auto found=preview.lookup(2,name);check(bool(found),"independent fixture inode missing");
            auto info=preview.inode(found.value());check(bool(info),"fixture inode metadata");
            std::array<u8,4096> content{};content.fill(0xa5);
            auto got=preview.read(found.value(),0,content);check(bool(got),"fixture data read");
            if(std::string_view(name)=="short")check(got.value()==4&&std::string_view(reinterpret_cast<char*>(content.data()),4)=="seed","fast symlink decode");
            else if(std::string_view(name)=="long")check(got.value()==80&&content[0]=='l'&&content[79]=='l',"block symlink decode");
            else {check(got.value()==4096,"sparse/unwritten size");for(usize i=0;i<got.value();++i)check(content[i]==0,"sparse/unwritten nonzero exposure");}
            ++cases;
        }
        const std::string long_name(255,'n');check(bool(preview.lookup(2,long_name)),"255-byte directory component");++cases;
        for (const bool use_extents : {false, true})
        {
            auto b = original; const auto at = inode_offset(b,14);
            check(r32(b,at+32)==0 || r32(b,at+32)==0x80000,"unexpected upstream symlink layout");
            const auto physical = r32(b,at+(r32(b,at+32) ? 60 : 40));
            for(usize i=40;i<100;++i)b[at+i]=0;
            w32(b,at+32,use_extents ? 0x80000 : 0);
            if (use_extents)
            {
                w16(b,at+40,0xf30a);w16(b,at+42,1);w16(b,at+44,4);
                w16(b,at+56,1);w32(b,at+60,physical);
            }
            else w32(b,at+40,physical);
            inode_crc(b,14,at);
            Device device(b);Ext4ReplayPreview<> extents;status(extents.open(device,{0,b.size()/512}),"extent symlink view");
            auto target=extents.lookup(2,"long");check(bool(target),"extent symlink lookup");std::array<u8,80> text{};
            auto got=extents.read(target.value(),0,text);check(got&&got.value()==80,"extent symlink read");
            for(auto byte:text)check(byte=='l',"extent symlink bytes");
            device.untouched();++cases;
        }
        {
            auto b=original;const usize at=inode_offset(b,16),physical=r32(b,at+60);
            for(usize i=0;i<4096;++i)b[physical*block_size(b)+i]=0xa5;
            Device device(b);Ext4ReplayPreview<> unwritten;status(unwritten.open(device,{0,b.size()/512}),"poisoned unwritten data view");
            std::array<u8,4096> bytes{};bytes.fill(0x5a);auto got=unwritten.read(16,0,bytes);
            check(got&&got.value()==4096,"unwritten data read");for(auto byte:bytes)check(byte==0,"unwritten backing bytes exposed");
            device.untouched();++cases;
        }

    }
    std::printf("PASS hosted ext4 replay preview block_size=%u dirty=%u cases=%zu actions=%zu reads=%zu writes=0 flushes=0\n",
                block_size(original),dirty,cases,preview.report().planned_actions,successful_reads);
    return 0;
}
