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

#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Plugins/OSMonitors/Support/ProcessExecutionDetector.h>
#include <s2e/Plugins/OSMonitors/Support/MemoryMap.h>
#include <s2e/Plugins/CRAX/Utils/StringUtil.h>

#include <algorithm>

#include "CRAX.h"

using namespace klee;

namespace s2e::plugins::crax {

#define CRAX_CONFIG_GET_BOOL(key, defaultValue) \
    (g_s2e->getConfig()->getBool(getConfigKey() + key, defaultValue))

#define CRAX_CONFIG_GET_STRING(key) \
    (g_s2e->getConfig()->getString(getConfigKey() + key))

S2E_DEFINE_PLUGIN(CRAX, "Modular Exploit Generation System", "", );

pybind11::scoped_interpreter CRAX::s_pybind11;
pybind11::module CRAX::s_pwnlib(pybind11::module::import("pwnlib.elf"));

CRAX::CRAX(S2E *s2e)
    : Plugin(s2e),
      beforeInstructionHooks(),
      afterInstructionHooks(),
      beforeSyscallHooks(),
      afterSyscallHooks(),
      exploitGenerationHooks(),
      onStateForkModuleDecide(),
      m_currentState(),
      m_linuxMonitor(),
      m_showInstructions(CRAX_CONFIG_GET_BOOL(".showInstructions", false)),
      m_showSyscalls(CRAX_CONFIG_GET_BOOL(".showSyscalls", true)),
      m_disableNativeForking(CRAX_CONFIG_GET_BOOL(".disableNativeForking", false)),
      m_register(*this),
      m_memory(*this),
      m_disassembler(*this),
      m_exploit(CRAX_CONFIG_GET_STRING(".elfFilename"),
                CRAX_CONFIG_GET_STRING(".libcFilename")),
      m_targetProcessPid(),
      m_scheduledAfterSyscallHooks(),
      m_allowedForkingStates(),
      m_modules(),
      m_readPrimitives(),
      m_writePrimitives() {}


void CRAX::initialize() {
    // Initialize CRAX++'s logging module.
    initCRAXLogging(this);
    m_register.initialize();
    m_memory.initialize();

    m_linuxMonitor = s2e()->getPlugin<LinuxMonitor>();

    m_linuxMonitor->onProcessLoad.connect(
            sigc::mem_fun(*this, &CRAX::onProcessLoad));

    // Install symbolic RIP handler.
    s2e()->getCorePlugin()->onSymbolicAddress.connect(
            sigc::mem_fun(*this, &CRAX::onSymbolicRip));

    s2e()->getCorePlugin()->onStateForkDecide.connect(
            sigc::mem_fun(*this, &CRAX::onStateForkDecide));

    // Initialize modules.
    ConfigFile *cfg = s2e()->getConfig();
    ConfigFile::string_list moduleNames = cfg->getStringList(getConfigKey() + ".modules");
    foreach2 (it, moduleNames.begin(), moduleNames.end()) {
        log<WARN>() << "initializing: " << *it << '\n';
        m_modules.push_back(Module::create(*this, *it));
    }
}


void CRAX::onSymbolicRip(S2EExecutionState *exploitableState,
                         ref<Expr> symbolicRip,
                         uint64_t concreteRip,
                         bool &concretize,
                         CorePlugin::symbolicAddressReason reason) {
    if (reason != CorePlugin::symbolicAddressReason::PC) {
        return;
    }

    // Set m_currentState to exploitableState.
    // All subsequent calls to reg() and mem() will operate on m_currentState.
    setCurrentState(exploitableState);

    log<WARN>()
        << "Detected symbolic RIP: " << hexval(concreteRip)
        << ", original value is: " << hexval(reg().readConcrete(Register::X64::RIP))
        << "\n";

    reg().setRipSymbolic(symbolicRip);

    // Dump CPU registers.
    reg().showRegInfo();

    // Dump virtual memory mappings.
    mem().showMapInfo();

    // Execute exploit generation hooks installed by the user.
    exploitGenerationHooks.emit();

    s2e()->getExecutor()->terminateState(*exploitableState, "End of exploit generation");
}

void CRAX::onProcessLoad(S2EExecutionState *state,
                         uint64_t cr3,
                         uint64_t pid,
                         const std::string &imageFileName) {
    setCurrentState(state);

    log<WARN>() << "onProcessLoad: " << imageFileName << "\n";

    if (imageFileName.find(m_exploit.getElfFilename()) != imageFileName.npos) {
        m_targetProcessPid = pid;

        m_linuxMonitor->onModuleLoad.connect(
                sigc::mem_fun(*this, &CRAX::onModuleLoad));

        s2e()->getCorePlugin()->onTranslateInstructionStart.connect(
                sigc::mem_fun(*this, &CRAX::onTranslateInstructionStart));

        s2e()->getCorePlugin()->onTranslateInstructionEnd.connect(
                sigc::mem_fun(*this, &CRAX::onTranslateInstructionEnd));
    }
}

void CRAX::onModuleLoad(S2EExecutionState *state,
                        const ModuleDescriptor &md) {
    setCurrentState(state);

    auto &os = log<WARN>();
    os << "onModuleLoad: " << md.Name << '\n';

    for (auto section : md.Sections) {
        section.name = md.Name;
        mem().getMappedSections().push_back(section);
    }

    // Resolve ELF base.
    //
    // Note that onModuleLoad() is triggered by load_elf_binary(),
    // so libc and other shared libraries have not yet been loaded
    // by the dynamic loader (ld-linux.so.2) at this point.
    //
    // See: github.com/S2E/s2e-linux-kernel: linux-4.9.3/fs/binfmt_elf.c
    if (md.Name == "target" && m_exploit.getElf().getChecksec().hasPIE) {
        auto mapInfo = mem().getMapInfo();
        m_exploit.getElf().setBase(mapInfo.begin()->start);
        log<WARN>() << "ELF loaded at: " << hexval(mapInfo.begin()->start) << '\n';
    }
}

void CRAX::onTranslateInstructionStart(ExecutionSignal *onInstructionExecute,
                                       S2EExecutionState *state,
                                       TranslationBlock *tb,
                                       uint64_t pc) {
    if (m_linuxMonitor->isKernelAddress(pc)) {
        return;
    }

    // Register the instruction hook which will be called
    // before the instruction is executed.
    onInstructionExecute->connect(
            sigc::mem_fun(*this, &CRAX::onExecuteInstructionStart));
}

void CRAX::onTranslateInstructionEnd(ExecutionSignal *onInstructionExecute,
                                     S2EExecutionState *state,
                                     TranslationBlock *tb,
                                     uint64_t pc) {
    if (m_linuxMonitor->isKernelAddress(pc)) {
        return;
    }

    // Register the instruction hook which will be called
    // after the instruction is executed.
    onInstructionExecute->connect(
            sigc::mem_fun(*this, &CRAX::onExecuteInstructionEnd));
}

void CRAX::onExecuteInstructionStart(S2EExecutionState *state,
                                     uint64_t pc) {
    setCurrentState(state);

    std::optional<Instruction> i = m_disassembler.disasm(pc);

    if (!i) {
        return;
    }

    if (m_showInstructions && !m_linuxMonitor->isKernelAddress(pc)) {
        log<INFO>() << hexval(i->address) << ": " << i->mnemonic << ' ' << i->opStr << '\n';
    }

    if (i->mnemonic == "syscall") {
        onExecuteSyscallStart(state, pc);
    }

    if (m_scheduledAfterSyscallHooks.size()) {
        auto it = m_scheduledAfterSyscallHooks.find(pc);
        if (it != m_scheduledAfterSyscallHooks.end()) {
            onExecuteSyscallEnd(state, pc, it->second);
            //m_scheduledAfterSyscallHooks.erase(pc);
        }
    }

    // Execute instruction hooks installed by the user.
    beforeInstructionHooks.emit(state, *i);
}

void CRAX::onExecuteInstructionEnd(S2EExecutionState *state,
                                   uint64_t pc) {
    setCurrentState(state);

    std::optional<Instruction> i = m_disassembler.disasm(pc);

    if (!i) {
        return;
    }

    // Execute instruction hooks installed by the user.
    afterInstructionHooks.emit(state, *i);
}

void CRAX::onExecuteSyscallStart(S2EExecutionState *state,
                                 uint64_t pc) {
    SyscallCtx syscall;
    syscall.ret = 0;
    syscall.nr = reg().readConcrete(Register::X64::RAX);
    syscall.arg1 = reg().readConcrete(Register::X64::RDI);
    syscall.arg2 = reg().readConcrete(Register::X64::RSI);
    syscall.arg3 = reg().readConcrete(Register::X64::RDX);
    syscall.arg4 = reg().readConcrete(Register::X64::R10);
    syscall.arg5 = reg().readConcrete(Register::X64::R8);
    syscall.arg6 = reg().readConcrete(Register::X64::R9);

    if (m_showSyscalls) {
        log<INFO>()
            << "syscall: " << hexval(syscall.nr) << " ("
            << hexval(syscall.arg1) << ", "
            << hexval(syscall.arg2) << ", "
            << hexval(syscall.arg3) << ", "
            << hexval(syscall.arg4) << ", "
            << hexval(syscall.arg5) << ", "
            << hexval(syscall.arg6) << '\n';
    }

    // Schedule the syscall hook to be called
    // after the instruction at `pc + 2` is executed.
    // Note: pc == state->regs()->getPc().
    m_scheduledAfterSyscallHooks[pc + 2] = syscall;

    // Execute syscall hooks installed by the user.
    beforeSyscallHooks.emit(state, m_scheduledAfterSyscallHooks[pc + 2]);
}

void CRAX::onExecuteSyscallEnd(S2EExecutionState *state,
                               uint64_t pc,
                               SyscallCtx &syscall) {
    // The kernel has finished serving the system call,
    // and the return value is now placed in RAX.
    syscall.ret = reg().readConcrete(Register::X64::RAX);

    // Execute syscall hooks installed by the user.
    afterSyscallHooks.emit(state, syscall);
}

void CRAX::onStateForkDecide(S2EExecutionState *state,
                             bool *allowForking) {
    // At this point, `*allowForking` is true by default.
    if (!m_disableNativeForking) {
        return;
    }

    // If the user has explicitly disabled all state forks done by S2E,
    // then we'll let CRAX's modules decide whether this fork should be done.
    onStateForkModuleDecide.emit(state, allowForking);

    // We'll also check if current state forking was requested by CRAX.
    // If yes, then `state` should be in `m_allowedForkingStates`.
    *allowForking |= m_allowedForkingStates.erase(state) == 1;
}


bool CRAX::isCallSiteOf(uint64_t pc, const std::string &symbol) const {
    std::optional<Instruction> i = m_disassembler.disasm(pc);
    assert(i && "isCallSiteOf(): Unable to disassemble the instruction");

    const uint64_t symbolPlt = m_exploit.getElf().getRuntimeAddress(symbol);
    return i->mnemonic == "call" && symbolPlt == std::stoull(i->opStr, nullptr, 16);
}

std::string CRAX::getBelongingSymbol(uint64_t instructionAddr) const {
    ELF::SymbolMap __s = m_exploit.getElf().symbols();
    std::vector<std::pair<std::string, uint64_t>> syms(__s.begin(), __s.end());

    std::sort(syms.begin(),
              syms.end(),
              [](const auto &p1, const auto &p2) { return p1.second < p2.second; });

    if (instructionAddr < syms.front().second) {
        log<WARN>()
            << "Unable to find which symbol " << hexval(instructionAddr)
            << " belongs to.\n";
        return "";
    }

    // Use binary search to find out which symbol `instructionAddr` belongs to.
    int left = 0;
    int right = syms.size() - 1;

    while (left < right) {
        int mid = left + (right + left) / 2;
        uint64_t addr = syms[mid].second;
        if (addr < instructionAddr) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    if (instructionAddr < syms[left].second) {
        left--;
    }
    return syms[left].first;
}

}  // namespace s2e::plugins::crax
