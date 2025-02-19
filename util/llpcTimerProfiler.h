/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
 /**
  ***********************************************************************************************************************
  * @file  llpcTimeProfiler.h
  * @brief LLPC header file: contains the definition of LLPC utility class TimerProfiler.
  ***********************************************************************************************************************
  */

#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Timer.h"

#include "llpc.h"
#include "llpcDebug.h"

namespace Llpc
{

class PassManager;

// =====================================================================================================================
// Enumerates the kinds of timer used to do profiling for LLPC compilation phases.
enum TimerKind : uint32_t
{
    TimerTranslate,  // Timer for translator
    TimerLower,      // Timer for SPIR-V lowering
    TimerLoadBc,     // Timer for loading LLVM bitcode
    TimerPatch,      // Timer for LLVM patching
    TimerOpt,        // Timer for LLVM optimization
    TimerCodeGen,    // Timer for backend code generation

    TimerCount
};

// =====================================================================================================================
// Represents a utility class for time profile, it wraps LLVM Timer and TimerGroup in internal.
class TimerProfiler
{
public:
    TimerProfiler(uint64_t hash64, const char* pDescriptionPrefix, uint32_t enableMask);

    ~TimerProfiler();

    void AddTimerStartStopPass(PassManager* pPassMgr, TimerKind timerKind, bool start);

    void StartStopTimer(TimerKind name, bool start);

    llvm::Timer* GetTimer(TimerKind timerKind);

    static const llvm::StringMap<llvm::TimeRecord>& GetDummyTimeRecords();

    // -----------------------------------------------------------------------------------------------------------------

    static const uint32_t PipelineTimerEnableMask = ((1 << TimerCount) - 1);
    static const uint32_t ShaderModuleTimerEnableMask = ((1 << TimerTranslate) | (1 << TimerLower));

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(TimerProfiler);

    // -----------------------------------------------------------------------------------------------------------------

    llvm::TimerGroup m_total;                // TimeGroup for total time
    llvm::TimerGroup m_phases;               // TimeGroup for each phase
    llvm::Timer m_wholeTimer;                // Whole timer
    llvm::Timer m_phaseTimers[TimerCount];   // Phase timer
};

} // Llpc
