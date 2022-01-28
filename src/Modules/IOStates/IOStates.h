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

#ifndef S2E_PLUGINS_CRAX_IO_STATES_H
#define S2E_PLUGINS_CRAX_IO_STATES_H

#include <s2e/S2EExecutionState.h>
#include <s2e/Plugins/CRAX/API/Disassembler.h>
#include <s2e/Plugins/CRAX/Modules/Module.h>

#include <array>
#include <map>
#include <string>
#include <vector>
#include <variant>
#include <utility>

namespace s2e::plugins::crax {

// This is an implementation of "IOState" from balsn's LAEG.
class IOStates : public Module {
public:
    enum LeakType {
        UNKNOWN,
        CODE,
        LIBC,
        HEAP,
        STACK,
        CANARY,
        LAST
    };

    struct InputStateInfo {
        std::vector<uint8_t> buf;
        uint64_t offset;
    };

    struct OutputStateInfo {
        bool valid;
        uint64_t bufIndex;
        uint64_t baseOffset;
        LeakType leakType;
    };

    using StateInfo = std::variant<InputStateInfo, OutputStateInfo>;


    class State : public ModuleState {
    public:
        State()
            : ModuleState(),
              leakableOffset(),
              lastInputStateInfoIdx(),
              lastInputStateInfoIdxBeforeFirstSymbolicRip(-1),
              currentLeakTargetIdx(),
              stateInfoList() {}

        virtual ~State() = default;

        static ModuleState *factory(Module *, CRAXState *) {
            return new State();
        }

        virtual ModuleState *clone() const override {
            return new State(*this);
        }

        std::string toString() const;

        // XXX: maybe make these data members private?
        uint64_t leakableOffset;
        uint32_t lastInputStateInfoIdx;
        uint32_t lastInputStateInfoIdxBeforeFirstSymbolicRip;
        uint32_t currentLeakTargetIdx;
        std::vector<StateInfo> stateInfoList;
    };


    IOStates();
    virtual ~IOStates() = default;

    virtual std::string toString() const override { return "IOStates"; }

    void print() const;
    uint64_t getCanary() const { return m_canary; }
    const std::vector<LeakType> &getLeakTargets() const { return m_leakTargets; }

    static const std::array<std::string, LeakType::LAST> s_leakTypes;

private:
    void initUserSpecifiedStateInfoList();

    void inputStateHookTopHalf(S2EExecutionState *inputState,
                               SyscallCtx &syscall);

    void inputStateHookBottomHalf(S2EExecutionState *inputState,
                                  const SyscallCtx &syscall);

    void outputStateHook(S2EExecutionState *outputState,
                         const SyscallCtx &syscall);


    void maybeInterceptStackCanary(S2EExecutionState *state,
                                   const Instruction &i);

    void onStackChkFailed(S2EExecutionState *state,
                          const Instruction &i);

    void onStateForkModuleDecide(S2EExecutionState *state,
                                 const klee::ref<klee::Expr> &__condition,
                                 bool &allowForking);

    void beforeExploitGeneration(S2EExecutionState *state);


    // Called at input states.
    [[nodiscard]]
    std::array<std::vector<uint64_t>, IOStates::LeakType::LAST>
    analyzeLeak(S2EExecutionState *inputState, uint64_t buf, uint64_t len);

    // Called at output states.
    [[nodiscard]]
    std::vector<IOStates::OutputStateInfo>
    detectLeak(S2EExecutionState *outputState, uint64_t buf, uint64_t len);

    LeakType getLeakType(const std::string &image) const;


    uint64_t m_canary;

    // The targets that must be leaked according to checksec.
    std::vector<LeakType> m_leakTargets;

    // User-specified stateInfoList.
    // If the user has defined this in s2e-config.lua, then the
    // "IOStates" module will not fork at input states. Instead,
    // it will follow the input offsets specified by the user.
    std::vector<StateInfo> m_userSpecifiedStateInfoList;
};

}  // namespace s2e::plugins::crax

#endif  // S2E_PLUGINS_CRAX_IO_STATES_H