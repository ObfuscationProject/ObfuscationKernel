// Checked locator parsing, before any recovery write.
    [[nodiscard]] Status startup_locator()
    {
        auto &s = *startup_workspace_;
        if (auto status = startup_read(1, s.locator_super_); !status) return status;
        if (ext4_checked::le16(s.locator_super_, 56) != 0xef53 ||
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{s.locator_super_}.first(1020)) != ext4_checked::le32(s.locator_super_, 1020) ||
            ext4_checked::le32(s.locator_super_, 24) || ext4_checked::le32(s.locator_super_, 4) != 32768 ||
            ext4_checked::le32(s.locator_super_, 0) != 8192 || ext4_checked::le32(s.locator_super_, 20) != 1 ||
            ext4_checked::le32(s.locator_super_, 32) != 8192 || ext4_checked::le32(s.locator_super_, 40) != 2048 ||
            ext4_checked::le16(s.locator_super_, 88) != 256 || ext4_checked::le32(s.locator_super_, 92) != 4 ||
            (ext4_checked::le32(s.locator_super_, 96) != 0x46 && ext4_checked::le32(s.locator_super_, 96) != 0x42) || ext4_checked::le32(s.locator_super_, 100) != 0x400 ||
            ext4_checked::le32(s.locator_super_, 224) != 8 || s.locator_super_[373] != 1)
            return Status::unsupported("reclaim recovery checksummed bootstrap superblock");
        for (usize index = 0; index < startup_locator_uuid_.size(); ++index) startup_locator_uuid_[index] = s.locator_super_[104 + index];
        if (auto status = startup_read(2, s.locator_descriptors_); !status) return status;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, startup_locator_uuid_);
        Block descriptor = s.locator_descriptors_;
        const auto checksum = ext4_checked::le16(descriptor, 30); descriptor[30] = descriptor[31] = 0;
        if (u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
                std::span<const u8>{descriptor}.first(32))) != checksum)
            return Status::io_error("reclaim recovery group-zero descriptor checksum");
        const u32 table = ext4_checked::le32(s.locator_descriptors_, 8);
        if (table < 3 || table >= 32768 - 512) return Status::invalid_argument("reclaim recovery journal table bounds");
        startup_journal_table_ = table + 1;
        if (auto status = startup_read(startup_journal_table_, s.locator_table_); !status) return status;
        const auto inode = std::span<const u8>{s.locator_table_}.subspan(3 * 256, 256);
        if (auto status = ext4_checked::inode_checksum(seed, 8, inode); !status) return status;
        startup_journal_blocks_ = ext4_checked::le16(inode, 56); startup_journal_first_ = ext4_checked::le32(inode, 60);
        if (ext4_checked::le16(inode, 0) != 0100600 || ext4_checked::le16(inode, 26) != 1 ||
            ext4_checked::le32(inode, 20) || ext4_checked::le32(inode, 32) != 0x80000 ||
            ext4_checked::le16(inode, 40) != 0xf30a || ext4_checked::le16(inode, 42) != 1 ||
            ext4_checked::le16(inode, 44) != 4 || ext4_checked::le16(inode, 46) ||
            ext4_checked::le32(inode, 52) || ext4_checked::le16(inode, 58) ||
            startup_journal_blocks_ < 1024 || startup_journal_blocks_ > 32768 || startup_journal_first_ < 3 || startup_journal_first_ >= 32768 ||
            startup_journal_blocks_ > 32768 - startup_journal_first_ || ext4_checked::le32(inode, 4) != startup_journal_blocks_ * 1024 ||
            ext4_checked::le32(inode, 28) != startup_journal_blocks_ * 2)
            return Status::unsupported("reclaim recovery contiguous journal inode");
        return {};
    }
