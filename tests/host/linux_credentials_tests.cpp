// Actual Linux adapter and PCB mutation with hosted checked-copy mappings.
#include "native_clock_model.hpp"

namespace
{
using user::Credentials;
using user::CredentialChange;
using user::IdChange;
using user::IdDomain;
constexpr u32 keep = user::unchanged_id;
unsigned forms{}, assertions{};

bool same(Credentials a, Credentials b)
{
    return a.uid==b.uid && a.euid==b.euid && a.suid==b.suid &&
           a.gid==b.gid && a.egid==b.egid && a.sgid==b.sgid && a.kernel_space==b.kernel_space;
}

void policy()
{
    const Credentials base{.uid=17,.gid=27,.euid=18,.egid=28,.suid=19,.sgid=29};
    const Credentials aggregate{.uid=17,.gid=27,.euid=18,.egid=28};
    CHECK(aggregate.suid==18 && aggregate.sgid==28);
    const Credentials positional{1000,1000,1000,1000,false};
    CHECK(!positional.kernel_space && positional.suid==1000 && positional.sgid==1000);
    CHECK(!user::change_credentials(positional,{IdDomain::user,IdChange::single,0}));
    const Credentials trusted{0,0,0,0,true};
    CHECK(trusted.kernel_space && trusted.suid==0 && trusted.sgid==0);
    CHECK(user::default_user_credentials().suid==1000 && user::default_user_credentials().sgid==1000);
    CHECK(user::root_credentials().suid==0 && user::kernel_credentials().sgid==0);
    for (auto domain : {IdDomain::user, IdDomain::group})
    {
        const u32 r=domain==IdDomain::user?17:27, e=r+1, s=r+2;
        struct Case { IdChange operation; u32 real,effective,saved; bool pass; u32 r,e,s; };
        const Case cases[]{
            {IdChange::single,r,keep,keep,true,r,r,s},
            {IdChange::single,s,keep,keep,true,r,s,s},
            {IdChange::single,e,keep,keep,false,0,0,0},
            {IdChange::single,42,keep,keep,false,0,0,0},
            {IdChange::single,keep,keep,keep,false,0,0,0},
            {IdChange::real_effective,keep,keep,keep,true,r,e,s},
            {IdChange::real_effective,r,keep,keep,true,r,e,e},
            {IdChange::real_effective,keep,r,keep,true,r,r,s},
            {IdChange::real_effective,keep,e,keep,true,r,e,e},
            {IdChange::real_effective,keep,s,keep,true,r,s,s},
            {IdChange::real_effective,e,r,keep,true,e,r,r},
            {IdChange::real_effective,s,keep,keep,false,0,0,0},
            {IdChange::real_effective,e,42,keep,false,0,0,0},
            {IdChange::real_effective_saved,keep,keep,keep,true,r,e,s},
            {IdChange::real_effective_saved,s,r,e,true,s,r,e},
            {IdChange::real_effective_saved,e,s,r,true,e,s,r},
            {IdChange::real_effective_saved,r,e,42,false,0,0,0},
            {IdChange::real_effective_saved,42,r,s,false,0,0,0},
            {IdChange::real_effective_saved,r,42,s,false,0,0,0},
        };
        for (const auto &test:cases)
        {
            const auto result=user::change_credentials(base,{domain,test.operation,test.real,test.effective,test.saved});
            CHECK(bool(result)==test.pass); ++assertions;
            if (result)
            {
                auto expected=base;
                if(domain==IdDomain::user){expected.uid=test.r;expected.euid=test.e;expected.suid=test.s;}
                else{expected.gid=test.r;expected.egid=test.e;expected.sgid=test.s;}
                CHECK(same(result.value(),expected));
            }
            else CHECK(result.status().code()==(test.operation==IdChange::single && test.real==keep?
                                               StatusCode::invalid_argument:StatusCode::permission));
        }
        for (auto operation:{IdChange::single,IdChange::real_effective,IdChange::real_effective_saved})
        {
            auto root=base;root.euid=0;
            auto result=user::change_credentials(root,{domain,operation,4000000000U,4000000001U,4000000002U});
            CHECK(result && !result.value().kernel_space);
            const auto next=result.value();
            CHECK((domain==IdDomain::user?next.uid:next.gid)==4000000000U);
            CHECK((domain==IdDomain::user?next.euid:next.egid)==(operation==IdChange::single?4000000000U:4000000001U));
            CHECK((domain==IdDomain::user?next.suid:next.sgid)==(operation==IdChange::single?4000000000U:
                  operation==IdChange::real_effective?4000000001U:4000000002U));
        }
    }
    auto group_root=base;group_root.egid=0;
    CHECK(!user::change_credentials(group_root,{IdDomain::group,IdChange::single,42}));
    CHECK(!user::change_credentials(user::kernel_credentials(),{IdDomain::user,IdChange::single,17}));
    CHECK(!user::change_credentials(base,{static_cast<IdDomain>(9),IdChange::single,17}));
    CHECK(!user::change_credentials(base,{IdDomain::user,static_cast<IdChange>(9),17}));
    auto reversible=user::change_credentials(user::root_credentials(),{IdDomain::user,IdChange::real_effective_saved,1000,1000,0});
    CHECK(reversible && reversible.value().suid==0 && reversible.value().euid==1000);
    auto restored=user::change_credentials(reversible.value(),{IdDomain::user,IdChange::single,0});
    CHECK(restored && restored.value().uid==1000 && restored.value().euid==0 && restored.value().suid==0);
    auto dropped=user::change_credentials(restored.value(),{IdDomain::user,IdChange::single,1000});
    CHECK(dropped && dropped.value().uid==1000 && dropped.value().suid==1000);
    CHECK(!user::change_credentials(dropped.value(),{IdDomain::user,IdChange::single,0}));
}

template <lu::Profile P>
void profile(Kernel &kernel,sched::ProcessId pid,sched::ProcessId other,usize number_slot,std::array<usize,3> slots,
             const std::array<u32,8> &expected)
{
    using C=lu::Codec<P>;using O=lu::Operation;
    const O old[]{O::setuid,O::setgid,O::setreuid,O::setregid,O::setresuid,O::setresgid,O::getresuid,O::getresgid};
    const O wide[]{O::setuid32,O::setgid32,O::setreuid32,O::setregid32,O::setresuid32,O::setresgid32,O::getresuid32,O::getresgid32};
    constexpr bool old16=P==lu::Profile::i386 || P==lu::Profile::arm32;
    constexpr std::array<u32,8> wide_numbers{213,214,203,204,208,210,209,211};
    auto *process=kernel.scheduler().find(pid);
    const std::array<std::pair<O,O>,4> scalar{{{O::getuid,O::getuid32},{O::geteuid,O::geteuid32},
        {O::getgid,O::getgid32},{O::getegid,O::getegid32}}};
    for (const auto &[old_scalar,wide_scalar]:scalar)
        for (const u32 value:{0x80000000U,0xfffff000U,0xfffff001U,0xfffffffeU})
        {
            const auto number=C::syscall_number(wide_scalar).has_value()?
                C::syscall_number(wide_scalar):C::syscall_number(old_scalar);
            CHECK(number);
            process->set_credentials({value,value,value,value,false});
            hw::UserFrame frame{};frame.registers[number_slot]=*number;
            bool done=true;
            const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
                [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
            CHECK(result==static_cast<i64>(value) && user::linux_bootstrap::complete<P>(frame,result));
            const auto raw=lu::NativeBridge<P>::snapshot(frame);
            CHECK(raw.integer[C::profile.return_register]==value);
            if constexpr(C::profile.error_convention!=lu::ErrorConvention::negative)
            {
                const auto returned=C::decode_return(raw);
                CHECK(returned && returned.value().value==value && returned.value().error==0);
            }
        }
    if constexpr(old16)
        for(const auto &[old_scalar,wide_scalar]:scalar)
            for(const u32 value:{65534U,65535U,65536U,0xfffffffeU})
            {
                static_cast<void>(wide_scalar);
                process->set_credentials({value,value,value,value,false});
                hw::UserFrame frame{};frame.registers[number_slot]=*C::syscall_number(old_scalar);
                bool done=true;
                const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
                    [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
                CHECK(result==(value>65535?65534:value) && user::linux_bootstrap::complete<P>(frame,result));
                const auto returned=lu::NativeBridge<P>::decode_return(frame);
                CHECK(returned && returned.value().error==0 && returned.value().value==static_cast<u64>(result));
                CHECK(kernel.scheduler().activate(other));
                frame.registers[number_slot]=*C::syscall_number(old_scalar);
                CHECK(user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
                    [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);})==-22);
                CHECK(kernel.scheduler().activate(pid));
            }
    if constexpr(C::profile.word_bytes==4 && C::profile.error_convention==lu::ErrorConvention::negative)
    {
        hw::UserFrame frame{};frame.registers[number_slot]=*C::syscall_number(O::write);
        CHECK(!user::linux_bootstrap::complete<P>(frame,0xfffff001ULL));
        CHECK(user::linux_bootstrap::complete<P>(frame,-14));
        const auto returned=lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned && returned.value().error==14);
    }
    auto call=[&](O operation,std::array<u64,3> args) {
        hw::UserFrame frame{};frame.registers[number_slot]=*C::syscall_number(operation);
        for(usize i=0;i<3;++i)frame.registers[slots[i]]=args[i];
        bool done=true;
        const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
        CHECK(done && user::linux_bootstrap::complete<P>(frame,result));
        auto returned=lu::NativeBridge<P>::decode_return(frame);CHECK(returned);
        const u32 error=result==-95?*C::errno_number(lu::Error::e_opnotsupp):static_cast<u32>(result<0?-result:0);
        CHECK(returned.value().error==error && returned.value().value==0);
        return result;
    };
    auto read=[&](usize offset,usize width) {
        u32 result=0;
        for(usize i=0;i<width;++i)result|=std::to_integer<u32>(mock::memory[offset+i])<<(8*(C::profile.big_endian?width-i-1:i));
        return result;
    };
    for(usize index=0;index<8;++index)
    {
        CHECK(C::syscall_number(old[index]) && *C::syscall_number(old[index])==expected[index]);
        CHECK(C::syscall_number(wide[index]).has_value()==old16);
        if constexpr(old16)CHECK(*C::syscall_number(wide[index])==wide_numbers[index]);
        for(usize version=0;version<(old16?2U:1U);++version)
        {
            ++forms;
            const O operation=version?wide[index]:old[index];
            const bool narrow=old16 && !version;
            const auto selected=user::linux_credentials::select<P>(*C::syscall_number(operation));
            CHECK(selected && selected->legacy16==narrow);
            const auto previous=process->credentials();
            const auto outputs=mock::outputs;
            CHECK(kernel.scheduler().activate(other));
            CHECK(call(operation,{42,43,44})==-22 && same(previous,process->credentials()) && mock::outputs==outputs);
            CHECK(kernel.scheduler().activate(pid));
            auto *caller=kernel.scheduler().current_thread();CHECK(caller);
            caller->state=sched::ProcessState::blocked;
            CHECK(call(operation,{42,43,44})==-22 && same(previous,process->credentials()) && mock::outputs==outputs);
            caller->state=sched::ProcessState::running;
            if(index<6)
            {
                process->set_credentials(user::root_credentials());
                const auto before_outputs=mock::outputs;
                CHECK(call(operation,{101,102,103})==0 && mock::outputs==before_outputs);
                const auto result=process->credentials();const bool group=index%2;
                CHECK((group?result.gid:result.uid)==101);
                CHECK((group?result.egid:result.euid)==(index<2?101:102));
                CHECK((group?result.sgid:result.suid)==(index<2?101:index<4?102:103));
                CHECK(!result.kernel_space);
                const Credentials denied{.uid=17,.gid=27,.euid=18,.egid=28,.suid=19,.sgid=29};
                process->set_credentials(denied);
                CHECK(call(operation,{42,43,44})==-1 && same(process->credentials(),denied));
                CHECK(call(operation,{keep,keep,keep})==(index<2?-22:0) && same(process->credentials(),denied));
                process->set_credentials(user::root_credentials());
                if(narrow)
                {
                    CHECK(call(operation,{0x12340065,0x12340066,0x12340067})==0);
                    CHECK((group?process->credentials().gid:process->credentials().uid)==101);
                }
                else if constexpr(C::profile.word_bytes==8)
                {
                    CHECK(call(operation,{(u64{0x12345678}<<32)|101,(u64{1}<<32)|102,(u64{1}<<32)|103})==0);
                    CHECK((group?process->credentials().gid:process->credentials().uid)==101);
                }
                const auto context=process->threads()[0].context;
                const auto peer=kernel.scheduler().create_thread(pid,context);CHECK(peer);
                const auto shared=process->credentials();
                CHECK(call(operation,{42,43,44})==-95 && same(process->credentials(),shared));
                CHECK(kernel.scheduler().exit_thread(pid,peer.value()));
                process->set_credentials(user::root_credentials());
                CHECK(call(operation,{71,72,73})==0);
                // Retired thread slots are removed so this fixture can reuse capacity.
                for(usize i=process->threads().size();i>1;--i)
                    if(process->threads()[i-1].state==sched::ProcessState::exited)CHECK(process->threads().erase_at(i-1));
            }
            else
            {
                const Credentials ids{.uid=65535,.gid=0x12345678,.euid=65536,.egid=29,.suid=0xfffffffe,.sgid=65535};
                process->set_credentials(ids);
                const usize width=narrow?2:4;
                const u32 values[3]={index==6?ids.uid:ids.gid,index==6?ids.euid:ids.egid,index==6?ids.suid:ids.sgid};
                auto value=[&](usize field){return narrow && values[field]>65535?65534:values[field];};
                const std::array<u64,3> addresses{hw::user_begin+129,hw::user_begin+145,hw::user_begin+161};
                mock::memory.fill(std::byte{0x55});auto before=mock::memory;
                CHECK(call(operation,addresses)==0 && same(process->credentials(),ids));
                for(usize f=0;f<3;++f)
                {
                    const usize offset=static_cast<usize>(addresses[f]-hw::user_begin);
                    CHECK(read(offset,width)==value(f));
                    for(usize i=0;i<width;++i)before[offset+i]=mock::memory[offset+i];
                }
                CHECK(mock::memory==before);
                for(usize fault=0;fault<3;++fault)
                {
                    mock::memory.fill(std::byte{0x55});mock::write_forbidden=addresses[fault];
                    const auto stores=mock::outputs;
                    CHECK(call(operation,addresses)==-14 && mock::outputs==stores+fault+1);
                    for(usize f=0;f<3;++f)
                        CHECK(read(addresses[f]-hw::user_begin,width)==(f<fault?value(f):narrow?0x5555U:0x55555555U));
                    CHECK(same(process->credentials(),ids));mock::write_forbidden=0;
                }
                mock::memory.fill(std::byte{0x55});
                CHECK(call(operation,{addresses[0],addresses[0],addresses[0]})==0 && read(129,width)==value(2));
                CHECK(call(operation,{0,addresses[1],addresses[2]})==-14);
                const auto end=hw::user_begin+mock::memory.size()-width;
                CHECK(call(operation,{end,end,end})==0 && read(mock::memory.size()-width,width)==value(2));
                CHECK(call(operation,{end+1,end,end})==-14);
            }
        }
    }
}

void lifecycle(Kernel &kernel,sched::ProcessId pid)
{
    auto *parent=kernel.scheduler().find(pid);
    const Credentials ids{.uid=17,.gid=27,.euid=18,.egid=28,.suid=19,.sgid=29};
    parent->set_credentials(ids);
    const auto child=kernel.scheduler().fork_process(pid,false);CHECK(child);
    auto *copy=kernel.scheduler().find(child.value());CHECK(copy && same(copy->credentials(),ids));
    copy->set_credentials(user::default_user_credentials());CHECK(same(parent->credentials(),ids));
    copy->set_credentials(ids);
    auto context=copy->threads()[0].context;
    auto invalid=context;invalid.architecture=static_cast<arch::Architecture>(255);
    CHECK(!kernel.scheduler().replace_image(child.value(),invalid,copy->memory_map()) && same(copy->credentials(),ids));
    CHECK(kernel.scheduler().replace_image(child.value(),context,copy->memory_map()));
    auto expected=ids;expected.suid=ids.euid;expected.sgid=ids.egid;
    CHECK(same(copy->credentials(),expected) && same(parent->credentials(),ids));
    CHECK(kernel.scheduler().exit_process(child.value(),0));
    CHECK(kernel.scheduler().wait_process(pid,child.value()));
    user::UserManager users;
    auto normal=users.credentials_for("user");CHECK(normal && normal.value().suid==1000 && normal.value().sgid==1000);
    CHECK(users.add_user(40001,40002,"saved-user","/saved-user"));
    auto account=users.credentials_for("saved-user");CHECK(account && account.value().suid==40001 && account.value().sgid==40002);
    CHECK(!users.can_switch(normal.value(),user::root_credentials()));
}
}

int main()
{
    policy();
    static Kernel kernel;CHECK(kernel.boot({}));
    auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto process=kernel.scheduler().create_user_process("saved-ids-model",context);
    const auto other=kernel.scheduler().create_user_process("saved-ids-other",context);CHECK(other);
    CHECK(process && kernel.posix().activate(process.value()));const auto pid=process.value();
    profile<lu::Profile::i386>(kernel,pid,other.value(),7,{4,6,5},{23,46,70,71,164,170,165,171});
    profile<lu::Profile::x86_64>(kernel,pid,other.value(),14,{9,10,11},{105,106,113,114,117,119,118,120});
    profile<lu::Profile::aarch64>(kernel,pid,other.value(),8,{0,1,2},{146,144,145,143,147,149,148,150});
    profile<lu::Profile::arm32>(kernel,pid,other.value(),7,{0,1,2},{23,46,70,71,164,170,165,171});
    profile<lu::Profile::rv64>(kernel,pid,other.value(),17,{10,11,12},{146,144,145,143,147,149,148,150});
    profile<lu::Profile::rv32>(kernel,pid,other.value(),17,{10,11,12},{146,144,145,143,147,149,148,150});
    profile<lu::Profile::loongarch64>(kernel,pid,other.value(),11,{4,5,6},{146,144,145,143,147,149,148,150});
    profile<lu::Profile::mips>(kernel,pid,other.value(),2,{4,5,6},{4023,4046,4070,4071,4185,4190,4186,4191});
    profile<lu::Profile::mips64>(kernel,pid,other.value(),2,{4,5,6},{5103,5104,5111,5112,5115,5117,5116,5118});
    profile<lu::Profile::ppc>(kernel,pid,other.value(),0,{3,4,5},{23,46,70,71,164,169,165,170});
    CHECK(forms==96 && assertions==38);lifecycle(kernel,pid);
    std::puts("SAVED_IDS_MODEL_PASS profiles=10 forms=96 policy_cases=38 checked_copy=1 lifecycle=1 guest=0");
}
