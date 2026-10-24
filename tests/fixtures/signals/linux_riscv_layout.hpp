#pragma once
#include <cstddef>
namespace linux_riscv_oracle {
struct rv32 {
static constexpr std::size_t frame_bytes = 944;
static constexpr std::size_t frame_alignment = 16;
static constexpr std::size_t uc = 128;
static constexpr std::size_t stack = 136;
static constexpr std::size_t stack_flags = 140;
static constexpr std::size_t stack_size = 144;
static constexpr std::size_t mask = 148;
static constexpr std::size_t context = 288;
static constexpr std::size_t pc = 288;
static constexpr std::size_t sp = 296;
static constexpr std::size_t tp = 304;
static constexpr std::size_t fp = 416;
static constexpr std::size_t fcsr = 672;
static constexpr std::size_t reserved = 932;
static constexpr std::size_t magic = 936;
static constexpr std::size_t record_size = 940;
static constexpr std::size_t fp_union_bytes = 528;
static constexpr std::size_t fp_d_bytes = 264;
static constexpr std::size_t info_bytes = 128;
static constexpr std::size_t info_pid = 12;
static constexpr std::size_t info_uid = 16;
static constexpr std::size_t register_bytes = 128;
static constexpr std::size_t uc_bytes = 816;
};
struct rv64 {
static constexpr std::size_t frame_bytes = 1088;
static constexpr std::size_t frame_alignment = 16;
static constexpr std::size_t uc = 128;
static constexpr std::size_t stack = 144;
static constexpr std::size_t stack_flags = 152;
static constexpr std::size_t stack_size = 160;
static constexpr std::size_t mask = 168;
static constexpr std::size_t context = 304;
static constexpr std::size_t pc = 304;
static constexpr std::size_t sp = 320;
static constexpr std::size_t tp = 336;
static constexpr std::size_t fp = 560;
static constexpr std::size_t fcsr = 816;
static constexpr std::size_t reserved = 1076;
static constexpr std::size_t magic = 1080;
static constexpr std::size_t record_size = 1084;
static constexpr std::size_t fp_union_bytes = 528;
static constexpr std::size_t fp_d_bytes = 264;
static constexpr std::size_t info_bytes = 128;
static constexpr std::size_t info_pid = 16;
static constexpr std::size_t info_uid = 20;
static constexpr std::size_t register_bytes = 256;
static constexpr std::size_t uc_bytes = 960;
};
}
