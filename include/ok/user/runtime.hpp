#pragma once
#include "ok/fs/archive.hpp"
namespace ok
{
class Kernel;
}
namespace ok::user::native
{
Status start(Kernel &kernel, fs::BootArchive archive);
bool step(Kernel &kernel);
void input(char value);
void release_process(Kernel &kernel, u64 pid);
} // namespace ok::user::native
