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

#include "DynamicRop.h"

using namespace klee;

namespace s2e::plugins::crax {

DynamicRop::DynamicRop()
    : Module(),
      m_currentConstraintGroup() {
    g_crax->beforeExploitGeneration.connect(
            sigc::mem_fun(*this, &DynamicRop::beforeExploitGeneration));
}


DynamicRop &DynamicRop::addConstraint(const DynamicRop::Constraint &c) {
    m_currentConstraintGroup.push_back(c);
    return *this;
}

void DynamicRop::commitConstraints() {
    auto modState = g_crax->getPluginModuleState(g_crax->getCurrentState(), this);
    assert(modState);

    modState->constraintsQueue.push(std::move(m_currentConstraintGroup));
}


void DynamicRop::applyNextConstraintGroup(S2EExecutionState &state) {
    auto modState = g_crax->getPluginModuleState(&state, this);
    assert(modState);

    if (modState->constraintsQueue.empty()) {
        log<WARN>() << "No more dynamic ROP constraints to apply.\n";
        return;
    }

    bool ok;
    bool hasRipConstraint = false;
    const ELF &elf = g_crax->getExploit().getElf();

    log<WARN>() << "Adding dynamic ROP constraints...\n";
    for (const auto &c : modState->constraintsQueue.front()) {
        if (const auto rc = std::get_if<RegisterConstraint>(&c)) {
            hasRipConstraint |= rc->reg == Register::X64::RIP;

            auto ce = dyn_cast<ConstantExpr>(rc->expr);
            ref<Expr> e1 = rc->expr;
            ref<Expr> e2 = rc->expr;

            // Try to rebase address `ce` to the user-specified elf base.
            // XXX: Currently it only checks whether `ce` is an ELF address.
            // We should probably do this for any other module/region.
            const auto &mapInfo = mem(&state).getMapInfo();
            auto it = mapInfo.find(ce->getZExtValue());
            bool isElfAddress = it != mapInfo.end() &&
                                (*it)->moduleName == VirtualMemoryMap::s_elfLabel;

            if (g_crax->getUserSpecifiedElfBase() && isElfAddress) {
                uint64_t userElfBase = g_crax->getUserSpecifiedElfBase();
                uint64_t rebasedAddress = elf.rebaseAddress(ce->getZExtValue(), userElfBase);
                e1 = ConstantExpr::create(rebasedAddress, Expr::Int64);
            }

            ok = RopChainBuilder::addRegisterConstraint(state, rc->reg, e1);
            reg().writeSymbolic(rc->reg, e2);

        } else if (const auto mc = std::get_if<MemoryConstraint>(&c)) {
            ok = RopChainBuilder::addMemoryConstraint(state, mc->addr, mc->expr);
            mem().writeSymbolic(mc->addr, mc->expr);
        }

        if (!ok) {
            g_s2e->getExecutor()->terminateState(state, "Dynamic ROP failed");
        }
    }

    modState->constraintsQueue.pop();

    // To make the target program restart at the address we've specified,
    // we need to throw a CpuExitException to invalidate the current translation block.
    if (hasRipConstraint) {
        throw CpuExitException();
    }
}

void DynamicRop::beforeExploitGeneration(S2EExecutionState *state) {
    assert(state);
    applyNextConstraintGroup(*state);
}

}  // namespace s2e::plugins::crax
