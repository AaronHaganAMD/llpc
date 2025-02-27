/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchEntryPointMutate.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchEntryPointMutate.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-entry-point-mutate"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llpcContext.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcIntrinsDefs.h"
#include "llpcPatchEntryPointMutate.h"
#include "llpcPipelineShaders.h"

using namespace llvm;
using namespace Llpc;

namespace llvm
{

namespace cl
{

// -vgpr-limit: maximum VGPR limit for this shader
static opt<uint32_t> VgprLimit("vgpr-limit", desc("Maximum VGPR limit for this shader"), init(0));

// -sgpr-limit: maximum SGPR limit for this shader
static opt<uint32_t> SgprLimit("sgpr-limit", desc("Maximum SGPR limit for this shader"), init(0));

// -waves-per-eu: the range of waves per EU for this shader
static opt<std::string> WavesPerEu("waves-per-eu",
                                   desc("The range of waves per EU for this shader"),
                                   value_desc("minVal,maxVal"),
                                   init(""));

// -inreg-esgs-lds-size: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent with PAL's
// GS on-chip behavior. In the future, if PAL allows hardcoded ES-GS LDS size, this option could be deprecated.
opt<bool> InRegEsGsLdsSize("inreg-esgs-lds-size",
                          desc("For GS on-chip, add esGsLdsSize in user data"),
                          init(true));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchEntryPointMutate::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for entry-point mutation
ModulePass* CreatePatchEntryPointMutate()
{
    return new PatchEntryPointMutate();
}

// =====================================================================================================================
PatchEntryPointMutate::PatchEntryPointMutate()
    :
    Patch(ID),
    m_hasTs(false),
    m_hasGs(false)
{
    initializePipelineStateWrapperPass(*PassRegistry::getPassRegistry());
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializePatchEntryPointMutatePass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchEntryPointMutate::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Entry-Point-Mutate\n");

    Patch::Init(&module);

    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);

    const uint32_t stageMask = m_pContext->GetShaderStageMask();
    m_hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                             ShaderStageToMask(ShaderStageTessEval))) != 0);
    m_hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    // Process each shader in turn, but not the copy shader.
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = ShaderStageVertex; shaderStage < ShaderStageNativeStageCount; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
            ProcessShader();
        }
    }

    return true;
}

// =====================================================================================================================
// Process a single shader
void PatchEntryPointMutate::ProcessShader()
{
    // Create new entry-point from the original one (mutate it)
    // TODO: We should mutate entry-point arguments instead of clone a new entry-point.
    uint64_t inRegMask = 0;
    FunctionType* pEntryPointTy = GenerateEntryPointType(&inRegMask);

    Function* pOrigEntryPoint = m_pEntryPoint;

    Function* pEntryPoint = Function::Create(pEntryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             "",
                                             m_pModule);
    pEntryPoint->setCallingConv(pOrigEntryPoint->getCallingConv());
    pEntryPoint->addFnAttr(Attribute::NoUnwind);
    pEntryPoint->takeName(pOrigEntryPoint);

    ValueToValueMapTy valueMap;
    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(pEntryPoint, pOrigEntryPoint, valueMap, false, retInsts);

    // Set Attributes on cloned function here as some are overwritten during CloneFunctionInto otherwise
    AttrBuilder builder;
    if (m_shaderStage == ShaderStageFragment)
    {
        auto& builtInUsage = m_pContext->GetShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
        SpiPsInputAddr spiPsInputAddr = {};

        spiPsInputAddr.bits.PERSP_SAMPLE_ENA     = ((builtInUsage.smooth && builtInUsage.sample) ||
                                                    builtInUsage.baryCoordSmoothSample);
        spiPsInputAddr.bits.PERSP_CENTER_ENA     = ((builtInUsage.smooth && builtInUsage.center) ||
                                                    builtInUsage.baryCoordSmooth);
        spiPsInputAddr.bits.PERSP_CENTROID_ENA   = ((builtInUsage.smooth && builtInUsage.centroid) ||
                                                    builtInUsage.baryCoordSmoothCentroid);
        spiPsInputAddr.bits.PERSP_PULL_MODEL_ENA = ((builtInUsage.smooth && builtInUsage.pullMode) ||
                                                    builtInUsage.baryCoordPullModel);
        spiPsInputAddr.bits.LINEAR_SAMPLE_ENA    = ((builtInUsage.noperspective && builtInUsage.sample) ||
                                                    builtInUsage.baryCoordNoPerspSample);
        spiPsInputAddr.bits.LINEAR_CENTER_ENA    = ((builtInUsage.noperspective && builtInUsage.center) ||
                                                    builtInUsage.baryCoordNoPersp);
        spiPsInputAddr.bits.LINEAR_CENTROID_ENA  = ((builtInUsage.noperspective && builtInUsage.centroid) ||
                                                    builtInUsage.baryCoordNoPerspCentroid);
        spiPsInputAddr.bits.POS_X_FLOAT_ENA      = builtInUsage.fragCoord;
        spiPsInputAddr.bits.POS_Y_FLOAT_ENA      = builtInUsage.fragCoord;
        spiPsInputAddr.bits.POS_Z_FLOAT_ENA      = builtInUsage.fragCoord;
        spiPsInputAddr.bits.POS_W_FLOAT_ENA      = builtInUsage.fragCoord;
        spiPsInputAddr.bits.FRONT_FACE_ENA       = builtInUsage.frontFacing;
        spiPsInputAddr.bits.ANCILLARY_ENA        = builtInUsage.sampleId;
        spiPsInputAddr.bits.SAMPLE_COVERAGE_ENA  = builtInUsage.sampleMaskIn;

        builder.addAttribute("InitialPSInputAddr", std::to_string(spiPsInputAddr.u32All));
    }

    // Set VGPR, SGPR, and wave limits
    uint32_t vgprLimit = 0;
    uint32_t sgprLimit = 0;
    std::string wavesPerEu;

    auto pShaderOptions = &(m_pContext->GetPipelineShaderInfo(m_shaderStage)->options);
    auto pResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);

    if ((pShaderOptions->vgprLimit != 0) && (pShaderOptions->vgprLimit != UINT32_MAX))
    {
        vgprLimit = pShaderOptions->vgprLimit;
    }
    else if (cl::VgprLimit != 0)
    {
        vgprLimit = cl::VgprLimit;
    }

    if (vgprLimit != 0)
    {
        builder.addAttribute("amdgpu-num-vgpr", std::to_string(vgprLimit));
        pResUsage->numVgprsAvailable = std::min(vgprLimit, pResUsage->numVgprsAvailable);
    }
    pResUsage->numVgprsAvailable = std::min(pResUsage->numVgprsAvailable,
                                           m_pContext->GetGpuProperty()->maxVgprsAvailable);

    if ((pShaderOptions->sgprLimit != 0) && (pShaderOptions->sgprLimit != UINT32_MAX))
    {
        sgprLimit = pShaderOptions->sgprLimit;
    }
    else if (cl::SgprLimit != 0)
    {
        sgprLimit = cl::SgprLimit;
    }

    if (sgprLimit != 0)
    {
        builder.addAttribute("amdgpu-num-sgpr", std::to_string(sgprLimit));
        pResUsage->numSgprsAvailable = std::min(sgprLimit, pResUsage->numSgprsAvailable);
    }
    pResUsage->numSgprsAvailable = std::min(pResUsage->numSgprsAvailable,
                                            m_pContext->GetGpuProperty()->maxSgprsAvailable);

    if (pShaderOptions->maxThreadGroupsPerComputeUnit != 0)
    {
        wavesPerEu = std::string("0,") + std::to_string(pShaderOptions->maxThreadGroupsPerComputeUnit);
    }
    else if (cl::WavesPerEu.empty() == false)
    {
        wavesPerEu = cl::WavesPerEu;
    }

    if (wavesPerEu.empty() == false)
    {
        builder.addAttribute("amdgpu-waves-per-eu", wavesPerEu);
    }

    AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
    pEntryPoint->addAttributes(attribIdx, builder);

    // NOTE: Remove "readnone" attribute for entry-point. If GS is emtry, this attribute will allow
    // LLVM optimization to remove sendmsg(GS_DONE). It is unexpected.
    if (pEntryPoint->hasFnAttribute(Attribute::ReadNone))
    {
        pEntryPoint->removeFnAttr(Attribute::ReadNone);
    }

    // Update attributes of new entry-point
    for (auto pArg = pEntryPoint->arg_begin(), pEnd = pEntryPoint->arg_end(); pArg != pEnd; ++pArg)
    {
        auto argIdx = pArg->getArgNo();
        if (inRegMask & (1ull << argIdx))
        {
            pArg->addAttr(Attribute::InReg);
        }
    }

    // Remove original entry-point
    pOrigEntryPoint->dropAllReferences();
    pOrigEntryPoint->eraseFromParent();
}

// =====================================================================================================================
// Checks whether the specified resource mapping node is active.
bool PatchEntryPointMutate::IsResourceNodeActive(
    const ResourceNode* pNode,               // [in] Resource mapping node
    bool isRootNode                          // TRUE if node is in root level
    ) const
{
    bool active = false;

    const ResourceUsage* pResUsage1 = m_pContext->GetShaderResourceUsage(m_shaderStage);
    const ResourceUsage* pResUsage2 = nullptr;

    const auto gfxIp = m_pContext->GetGfxIpVersion();
    if (gfxIp.major >= 9)
    {
        // NOTE: For LS-HS/ES-GS merged shader, resource mapping nodes of the two shader stages are merged as a whole.
        // So we have to check activeness of both shader stages at the same time. Here, we determine the second shader
       // stage and get its resource usage accordingly.
        uint32_t stageMask = m_pContext->GetShaderStageMask();
        const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                            ShaderStageToMask(ShaderStageTessEval))) != 0);
        const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        if (hasTs || hasGs)
        {
            const auto shaderStage1 = m_shaderStage;
            auto shaderStage2 = ShaderStageInvalid;

            if (shaderStage1 == ShaderStageVertex)
            {
                shaderStage2 = hasTs ? ShaderStageTessControl :
                                       (hasGs ? ShaderStageGeometry : ShaderStageInvalid);
            }
            else if (shaderStage1 == ShaderStageTessControl)
            {
                shaderStage2 = ShaderStageVertex;
            }
            else if (shaderStage1 == ShaderStageTessEval)
            {
                shaderStage2 = hasGs ? ShaderStageGeometry : ShaderStageInvalid;
            }
            else if (shaderStage1 == ShaderStageGeometry)
            {
                shaderStage2 = hasTs ? ShaderStageTessEval : ShaderStageVertex;
            }

            if (shaderStage2 != ShaderStageInvalid)
            {
                pResUsage2 = m_pContext->GetShaderResourceUsage(shaderStage2);
            }
        }
    }

    if ((pNode->type == ResourceMappingNodeType::PushConst) && isRootNode)
    {
        active = (pResUsage1->pushConstSizeInBytes > 0);
        if ((active == false) && (pResUsage2 != nullptr))
        {
            active = (pResUsage2->pushConstSizeInBytes > 0);
        }
    }
    else if (pNode->type == ResourceMappingNodeType::DescriptorTableVaPtr)
    {
        // Check if any contained descriptor node is active
        for (uint32_t i = 0; i < pNode->innerTable.size(); ++i)
        {
            if (IsResourceNodeActive(&pNode->innerTable[i], false))
            {
                active = true;
                break;
            }
        }
    }
    else if (pNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr)
    {
        // NOTE: We assume indirect user data is always active.
        active = true;
    }
    else if (pNode->type == ResourceMappingNodeType::StreamOutTableVaPtr)
    {
        active = true;
    }
    else
    {
        LLPC_ASSERT((pNode->type != ResourceMappingNodeType::DescriptorTableVaPtr) &&
                    (pNode->type != ResourceMappingNodeType::IndirectUserDataVaPtr));

        DescriptorPair descPair = {};
        descPair.descSet = pNode->set;
        descPair.binding = pNode->binding;

        active = (pResUsage1->descPairs.find(descPair.u64All) != pResUsage1->descPairs.end());
        if ((active == false) && (pResUsage2 != nullptr))
        {
            active = (pResUsage2->descPairs.find(descPair.u64All) != pResUsage2->descPairs.end());
        }
    }

    return active;
}

// =====================================================================================================================
// Generates the type for the new entry-point based on already-collected info in LLPC context.
FunctionType* PatchEntryPointMutate::GenerateEntryPointType(
    uint64_t* pInRegMask  // [out] "Inreg" bit mask for the arguments
    ) const
{
    uint32_t argIdx = 0;
    uint32_t userDataIdx = 0;
    std::vector<Type*> argTys;

    auto userDataNodes = m_pPipelineState->GetUserDataNodes();
    auto pIntfData = m_pContext->GetShaderInterfaceData(m_shaderStage);
    auto pResUsage = m_pContext->GetShaderResourceUsage(m_shaderStage);

    // Global internal table
    argTys.push_back(m_pContext->Int32Ty());
    *pInRegMask |= (1ull << (argIdx++));
    ++userDataIdx;

    // TODO: We need add per shader table per real usage after switch to PAL new interface.
    //if (pResUsage->perShaderTable)
    {
        argTys.push_back(m_pContext->Int32Ty());
        *pInRegMask |= (1ull << (argIdx++));
        ++userDataIdx;
    }

    auto& builtInUsage = pResUsage->builtInUsage;
    auto& entryArgIdxs = pIntfData->entryArgIdxs;
    entryArgIdxs.initialized = true;

    // Estimated available user data count
    uint32_t maxUserDataCount = m_pContext->GetGpuProperty()->maxUserDataCount;
    uint32_t availUserDataCount = maxUserDataCount - userDataIdx;
    uint32_t requiredRemappedUserDataCount = 0; // Maximum required user data
    uint32_t requiredUserDataCount = 0;         // Maximum required user data without remapping
    bool useFixedLayout = (m_shaderStage == ShaderStageCompute);
    bool reserveVbTable = false;
    bool reserveStreamOutTable = false;
    bool reserveEsGsLdsSize = false;

    if (userDataNodes.size() > 0)
    {
        for (uint32_t i = 0; i < userDataNodes.size(); ++i)
        {
            auto pNode = &userDataNodes[i];
             // NOTE: Per PAL request, the value of IndirectTableEntry is the node offset + 1.
             // and indirect user data should not be counted in possible spilled user data.
            if (pNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr)
            {
                // Only the vertex shader needs a vertex buffer table.
                if (m_shaderStage == ShaderStageVertex)
                {
                    reserveVbTable = true;
                }
                else if (m_pContext->GetGfxIpVersion().major >= 9)
                {
                    // On GFX9+, the shader stage that the vertex shader is merged in to needs a vertex buffer
                    // table, to ensure that the merged shader gets one.
                    if ((m_shaderStage == ShaderStageTessControl) ||
                        ((m_shaderStage == ShaderStageGeometry) && (m_hasTs == false)))
                    {
                        reserveVbTable = true;
                    }
                }
                continue;
            }

            if (pNode->type == ResourceMappingNodeType::StreamOutTableVaPtr)
            {
                // Only the last shader stage before fragment (ignoring copy shader) needs a stream out table.
                if ((m_pContext->GetShaderStageMask() &
                     (ShaderStageToMask(ShaderStageFragment) - ShaderStageToMask(m_shaderStage))) ==
                    ShaderStageToMask(m_shaderStage))
                {
                    reserveStreamOutTable = true;
                }
                else if (m_pContext->GetGfxIpVersion().major >= 9)
                {
                    // On GFX9+, the shader stage that the last shader is merged in to needs a stream out
                    // table, to ensure that the merged shader gets one.
                    if ((m_shaderStage == ShaderStageTessEval) ||
                        ((m_shaderStage == ShaderStageVertex) && (m_hasTs == false)))
                    {
                        reserveStreamOutTable = true;
                    }
                }
                continue;
            }

            if (IsResourceNodeActive(pNode, true) == false)
            {
                continue;
            }

            if (pNode->type == ResourceMappingNodeType::PushConst)
            {
                pIntfData->pushConst.resNodeIdx = i;
            }

            requiredUserDataCount = std::max(requiredUserDataCount, pNode->offsetInDwords + pNode->sizeInDwords);
            requiredRemappedUserDataCount += pNode->sizeInDwords;
        }
    }

    bool enableMultiView = false;
    if (m_shaderStage != ShaderStageCompute)
    {
        enableMultiView = (static_cast<const GraphicsPipelineBuildInfo*>(
            m_pContext->GetPipelineBuildInfo()))
            ->iaState.enableMultiView;
    }

#if LLPC_BUILD_GFX10
    const bool enableNgg = m_pContext->IsGraphics() ? m_pContext->GetNggControl()->enableNgg : false;
#endif

    switch (m_shaderStage)
    {
    case ShaderStageVertex:
    case ShaderStageTessControl:
        {
            if (enableMultiView)
            {
                availUserDataCount -= 1;
            }

            // Reserve register for "IndirectUserDataVaPtr"
            if (reserveVbTable)
            {
                availUserDataCount -= 1;
            }

            // Reserve for stream-out table
            if (reserveStreamOutTable)
            {
                availUserDataCount -= 1;
            }

            // NOTE: On GFX9+, Vertex shader (LS) and tessellation control shader (HS) are merged into a single shader.
            // The user data count of tessellation control shader should be same as vertex shader.
            auto pCurrResUsage = pResUsage;
            if ((m_pContext->GetGfxIpVersion().major >= 9) &&
                (m_shaderStage == ShaderStageTessControl) &&
                (m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)))
            {
                pCurrResUsage = m_pContext->GetShaderResourceUsage(ShaderStageVertex);
            }

            if (pCurrResUsage->builtInUsage.vs.baseVertex || pCurrResUsage->builtInUsage.vs.baseInstance)
            {
                availUserDataCount -= 2;
            }

            if (pCurrResUsage->builtInUsage.vs.drawIndex)
            {
                availUserDataCount -= 1;
            }

            // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
            // with PAL's GS on-chip behavior (VS is in NGG primitive shader).
            const auto gfxIp = m_pContext->GetGfxIpVersion();
            if (((gfxIp.major >= 9) && (m_pContext->IsGsOnChip() && cl::InRegEsGsLdsSize))
#if LLPC_BUILD_GFX10
                || (enableNgg && (m_hasTs == false))
#endif
                )
            {
                availUserDataCount -= 1;
                reserveEsGsLdsSize = true;
            }
            break;
        }
    case ShaderStageTessEval:
        {
            if (enableMultiView)
            {
                availUserDataCount -= 1;
            }

            // Reserve for stream-out table
            if (reserveStreamOutTable)
            {
                availUserDataCount -= 1;
            }

#if LLPC_BUILD_GFX10
            // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
            // with PAL's GS on-chip behavior (TES is in NGG primitive shader).
            if (enableNgg)
            {
                availUserDataCount -= 1;
                reserveEsGsLdsSize = true;
            }
#endif
            break;
        }
    case ShaderStageGeometry:
        {
            if (enableMultiView)
            {
                availUserDataCount -= 1;
            }

            // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
            // with PAL's GS on-chip behavior. i.e. GS is GFX8
            if ((m_pContext->IsGsOnChip() && cl::InRegEsGsLdsSize)
#if LLPC_BUILD_GFX10
                || enableNgg
#endif
                )
            {
                // NOTE: Add a dummy "inreg" argument for ES-GS LDS size, this is to keep consistent
                // with PAL's GS on-chip behavior.
                availUserDataCount -= 1;
                reserveEsGsLdsSize = true;
            }

            break;
        }
    case ShaderStageFragment:
        {
            // Do nothing
            break;
        }
    case ShaderStageCompute:
        {
            // Emulate gl_NumWorkGroups via user data registers
            if (builtInUsage.cs.numWorkgroups)
            {
                availUserDataCount -= 2;
            }
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    // NOTE: We have to spill user data to memory when available user data is less than required.
    bool needSpill = false;
    if (useFixedLayout)
    {
        LLPC_ASSERT(m_shaderStage == ShaderStageCompute);
        needSpill = (requiredUserDataCount > InterfaceData::MaxCsUserDataCount);
        availUserDataCount = InterfaceData::MaxCsUserDataCount;
    }
    else
    {
        needSpill = (requiredRemappedUserDataCount > availUserDataCount);
        pIntfData->spillTable.offsetInDwords = InvalidValue;
        if (needSpill)
        {
            // Spill table need an addtional user data
            --availUserDataCount;
        }
    }

    // Allocate register for stream-out buffer table
    if (reserveStreamOutTable)
    {
        for (uint32_t i = 0; i < userDataNodes.size(); ++i)
        {
            auto pNode = &userDataNodes[i];
            if (pNode->type == ResourceMappingNodeType::StreamOutTableVaPtr)
            {
                argTys.push_back(m_pContext->Int32Ty());
                LLPC_ASSERT(pNode->sizeInDwords == 1);
                switch (m_shaderStage)
                {
                case ShaderStageVertex:
                    {
                        pIntfData->userDataUsage.vs.streamOutTablePtr = userDataIdx;
                        pIntfData->entryArgIdxs.vs.streamOutData.tablePtr = argIdx;
                        break;
                    }
                case ShaderStageTessEval:
                    {
                        pIntfData->userDataUsage.tes.streamOutTablePtr = userDataIdx;
                        pIntfData->entryArgIdxs.tes.streamOutData.tablePtr = argIdx;
                        break;
                    }
                // Allocate dummpy stream-out register for Geometry shader
                case ShaderStageGeometry:
                    {
                        break;
                    }
                default:
                    {
                        LLPC_NEVER_CALLED();
                        break;
                    }
                }
                *pInRegMask |= (1ull << (argIdx++));
                ++userDataIdx;
                break;
            }
        }
    }

    // Descriptor table and vertex buffer table
    uint32_t actualAvailUserDataCount = 0;
    for (uint32_t i = 0; i < userDataNodes.size(); ++i)
    {
        auto pNode = &userDataNodes[i];

        // "IndirectUserDataVaPtr" can't be spilled, it is treated as internal user data
        if (pNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr)
        {
            continue;
        }

        if (pNode->type == ResourceMappingNodeType::StreamOutTableVaPtr)
        {
            continue;
        }

        if (IsResourceNodeActive(pNode, true) == false)
        {
            continue;
        }

        if (useFixedLayout)
        {
            // NOTE: For fixed user data layout (for compute shader), we could not pack those user data and dummy
            // entry-point arguments are added once DWORD offsets of user data are not continuous.
           LLPC_ASSERT(m_shaderStage == ShaderStageCompute);

            while ((userDataIdx < (pNode->offsetInDwords + InterfaceData::CsStartUserData)) &&
                   (userDataIdx < (availUserDataCount + InterfaceData::CsStartUserData)))
            {
                argTys.push_back(m_pContext->Int32Ty());
                *pInRegMask |= (1ull << argIdx++);
                ++userDataIdx;
                ++actualAvailUserDataCount;
            }
        }

        if (actualAvailUserDataCount + pNode->sizeInDwords <= availUserDataCount)
        {
            // User data isn't spilled
            LLPC_ASSERT(i < InterfaceData::MaxDescTableCount);
            pIntfData->entryArgIdxs.resNodeValues[i] = argIdx;
            *pInRegMask |= (1ull << (argIdx++));
            actualAvailUserDataCount += pNode->sizeInDwords;
            switch (pNode->type)
            {
            case ResourceMappingNodeType::DescriptorTableVaPtr:
                {
                    argTys.push_back(m_pContext->Int32Ty());

                    LLPC_ASSERT(pNode->sizeInDwords == 1);

                    pIntfData->userDataMap[userDataIdx] = pNode->offsetInDwords;
                    ++userDataIdx;
                    break;
                }

            case ResourceMappingNodeType::DescriptorResource:
            case ResourceMappingNodeType::DescriptorSampler:
            case ResourceMappingNodeType::DescriptorTexelBuffer:
            case ResourceMappingNodeType::DescriptorFmask:
            case ResourceMappingNodeType::DescriptorBuffer:
            case ResourceMappingNodeType::PushConst:
            case ResourceMappingNodeType::DescriptorBufferCompact:
                {
                    argTys.push_back(VectorType::get(m_pContext->Int32Ty(), pNode->sizeInDwords));
                    for (uint32_t j = 0; j < pNode->sizeInDwords; ++j)
                    {
                        pIntfData->userDataMap[userDataIdx + j] = pNode->offsetInDwords + j;
                    }
                    userDataIdx += pNode->sizeInDwords;
                    break;
                }
            default:
                {
                    LLPC_NEVER_CALLED();
                    break;
                }
            }
        }
        else if (needSpill && (pIntfData->spillTable.offsetInDwords == InvalidValue))
        {
            pIntfData->spillTable.offsetInDwords = pNode->offsetInDwords;
        }
    }

    // Internal user data
    if (needSpill && useFixedLayout)
    {
        // Add spill table
        LLPC_ASSERT(pIntfData->spillTable.offsetInDwords != InvalidValue);
        LLPC_ASSERT(userDataIdx <= (InterfaceData::MaxCsUserDataCount + InterfaceData::CsStartUserData));
        while (userDataIdx <= (InterfaceData::MaxCsUserDataCount + InterfaceData::CsStartUserData))
        {
            argTys.push_back(m_pContext->Int32Ty());
            *pInRegMask |= (1ull << argIdx++);
            ++userDataIdx;
        }
        pIntfData->userDataUsage.spillTable = userDataIdx - 1;
        pIntfData->entryArgIdxs.spillTable = argIdx - 1;

        pIntfData->spillTable.sizeInDwords = requiredUserDataCount;
    }

    switch (m_shaderStage)
    {
    case ShaderStageVertex:
    case ShaderStageTessControl:
        {
	    // NOTE: On GFX9+, Vertex shader (LS) and tessellation control shader (HS) are merged into a single shader.
	    // The user data count of tessellation control shader should be same as vertex shader.
            auto pCurrIntfData = pIntfData;
            auto pCurrResUsage = pResUsage;

            if ((m_pContext->GetGfxIpVersion().major >= 9) &&
                (m_shaderStage == ShaderStageTessControl) &&
                (m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)))
            {
                pCurrIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
                pCurrResUsage = m_pContext->GetShaderResourceUsage(ShaderStageVertex);
            }

            // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
            // merged shader, we place it prior to any other special user data.
            if (enableMultiView)
            {
                argTys.push_back(m_pContext->Int32Ty()); // View Index
                entryArgIdxs.vs.viewIndex = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pCurrIntfData->userDataUsage.vs.viewIndex = userDataIdx;
                ++userDataIdx;
            }

            if (reserveEsGsLdsSize)
            {
                argTys.push_back(m_pContext->Int32Ty());
                *pInRegMask |= (1ull << (argIdx++));
                pCurrIntfData->userDataUsage.vs.esGsLdsSize = userDataIdx;
                ++userDataIdx;
            }

            for (uint32_t i = 0; i < userDataNodes.size(); ++i)
            {
                auto pNode = &userDataNodes[i];
                if (pNode->type == ResourceMappingNodeType::IndirectUserDataVaPtr)
                {
                    argTys.push_back(m_pContext->Int32Ty());
                    LLPC_ASSERT(pNode->sizeInDwords == 1);
                    pCurrIntfData->userDataUsage.vs.vbTablePtr = userDataIdx;
                    pCurrIntfData->entryArgIdxs.vs.vbTablePtr = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));
                    ++userDataIdx;
                    break;
                }
            }

            if (pCurrResUsage->builtInUsage.vs.baseVertex || pCurrResUsage->builtInUsage.vs.baseInstance)
            {
                argTys.push_back(m_pContext->Int32Ty()); // Base vertex
                entryArgIdxs.vs.baseVertex = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pCurrIntfData->userDataUsage.vs.baseVertex = userDataIdx;
                ++userDataIdx;

                argTys.push_back(m_pContext->Int32Ty()); // Base instance
                entryArgIdxs.vs.baseInstance = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pCurrIntfData->userDataUsage.vs.baseInstance = userDataIdx;
                ++userDataIdx;
            }

            if (pCurrResUsage->builtInUsage.vs.drawIndex)
            {
                argTys.push_back(m_pContext->Int32Ty()); // Draw index
                entryArgIdxs.vs.drawIndex = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pCurrIntfData->userDataUsage.vs.drawIndex = userDataIdx;
                ++userDataIdx;
            }

            break;
        }
    case ShaderStageTessEval:
        {
            // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
            // merged shader, we place it prior to any other special user data.
            if (enableMultiView)
            {
                argTys.push_back(m_pContext->Int32Ty()); // View Index
                entryArgIdxs.tes.viewIndex = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pIntfData->userDataUsage.tes.viewIndex = userDataIdx;
                ++userDataIdx;
            }

#if LLPC_BUILD_GFX10
            if (reserveEsGsLdsSize)
            {
                argTys.push_back(m_pContext->Int32Ty());
                *pInRegMask |= (1ull << (argIdx++));
                pIntfData->userDataUsage.tes.esGsLdsSize = userDataIdx;
                ++userDataIdx;
            }
#endif
            break;
        }
    case ShaderStageGeometry:
        {
            // NOTE: The user data to emulate gl_ViewIndex is somewhat common. To make it consistent for GFX9
            // merged shader, we place it prior to any other special user data.
            if (enableMultiView)
            {
                argTys.push_back(m_pContext->Int32Ty()); // View Index
                entryArgIdxs.gs.viewIndex = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
                pIntfData->userDataUsage.gs.viewIndex = userDataIdx;
                ++userDataIdx;
            }

            if (reserveEsGsLdsSize)
            {
                argTys.push_back(m_pContext->Int32Ty());
                *pInRegMask |= (1ull << (argIdx++));
                pIntfData->userDataUsage.gs.esGsLdsSize = userDataIdx;
                ++userDataIdx;
            }
            break;
        }
    case ShaderStageCompute:
        {
            // Emulate gl_NumWorkGroups via user data registers
            if (builtInUsage.cs.numWorkgroups)
            {
                // NOTE: Pointer must be placed in even index according to LLVM backend compiler.
                if ((userDataIdx % 2) != 0)
                {
                    argTys.push_back(m_pContext->Int32Ty());
                    entryArgIdxs.cs.workgroupId = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));
                    userDataIdx += 1;
                }

                auto pNumWorkgroupsPtrTy = PointerType::get(m_pContext->Int32x3Ty(), ADDR_SPACE_CONST);
                argTys.push_back(pNumWorkgroupsPtrTy); // NumWorkgroupsPtr
                entryArgIdxs.cs.numWorkgroupsPtr = argIdx;
                pIntfData->userDataUsage.cs.numWorkgroupsPtr = userDataIdx;
                *pInRegMask |= (1ull << (argIdx++));
                userDataIdx += 2;
            }
            break;
        }
    case ShaderStageFragment:
        {
            // Do nothing
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    if (needSpill && (useFixedLayout == false))
    {
        argTys.push_back(m_pContext->Int32Ty());
        *pInRegMask |= (1ull << argIdx);

        pIntfData->userDataUsage.spillTable = userDataIdx++;
        pIntfData->entryArgIdxs.spillTable = argIdx++;

        pIntfData->spillTable.sizeInDwords = requiredUserDataCount;
    }
    pIntfData->userDataCount = userDataIdx;

    const auto& xfbStrides = pResUsage->inOutUsage.xfbStrides;

    const bool enableXfb = pResUsage->inOutUsage.enableXfb;

    // NOTE: Here, we start to add system values, they should be behind user data.
    switch (m_shaderStage)
    {
    case ShaderStageVertex:
        {
            if (m_hasGs && (m_hasTs == false))   // VS acts as hardware ES
            {
                argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset
                entryArgIdxs.vs.esGsOffset = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
            }
            else if ((m_hasGs == false) && (m_hasTs == false))  // VS acts as hardware VS
            {
                if (enableXfb)  // If output to stream-out buffer
                {
                    argTys.push_back(m_pContext->Int32Ty()); // Stream-out info (ID, vertex count, enablement)
                    entryArgIdxs.vs.streamOutData.streamInfo = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));

                    argTys.push_back(m_pContext->Int32Ty()); // Stream-out write Index
                    entryArgIdxs.vs.streamOutData.writeIndex = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));

                    for (uint32_t i = 0; i < MaxTransformFeedbackBuffers; ++i)
                    {
                        if (xfbStrides[i] > 0)
                        {
                            argTys.push_back(m_pContext->Int32Ty()); // Stream-out offset
                            entryArgIdxs.vs.streamOutData.streamOffsets[i] = argIdx;
                            *pInRegMask |= (1ull << (argIdx++));
                        }
                    }
                }
            }

            // NOTE: Order of these arguments could not be changed. The rule is very similar to function default
            // parameters: vertex ID [, relative vertex ID, primitive ID [, instance ID]]
            auto nextShaderStage = m_pContext->GetNextShaderStage(ShaderStageVertex);
            // NOTE: For tessellation control shader, we always need relative vertex ID.
            if (builtInUsage.vs.vertexIndex || builtInUsage.vs.primitiveId || builtInUsage.vs.instanceIndex ||
                (nextShaderStage == ShaderStageTessControl))
            {
                argTys.push_back(m_pContext->Int32Ty()); // Vertex ID
                entryArgIdxs.vs.vertexId = argIdx++;
            }

            if (builtInUsage.vs.primitiveId || builtInUsage.vs.instanceIndex ||
                (nextShaderStage == ShaderStageTessControl))
            {
                // NOTE: For tessellation control shader, we always need relative vertex ID.
                argTys.push_back(m_pContext->Int32Ty()); // Relative vertex ID (auto index)
                entryArgIdxs.vs.relVertexId = argIdx++;

                argTys.push_back(m_pContext->Int32Ty()); // Primitive ID
                entryArgIdxs.vs.primitiveId = argIdx++;
            }

            if (builtInUsage.vs.instanceIndex)
            {
                argTys.push_back(m_pContext->Int32Ty()); // Instance ID
                entryArgIdxs.vs.instanceId = argIdx++;
            }

            break;
        }
    case ShaderStageTessControl:
        {
            if (m_pContext->IsTessOffChip())
            {
                argTys.push_back(m_pContext->Int32Ty()); // Off-chip LDS buffer base
                entryArgIdxs.tcs.offChipLdsBase = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
            }

            argTys.push_back(m_pContext->Int32Ty()); // TF buffer base
            entryArgIdxs.tcs.tfBufferBase = argIdx;
            *pInRegMask |= (1ull << (argIdx++));

            argTys.push_back(m_pContext->Int32Ty()); // Patch ID
            entryArgIdxs.tcs.patchId = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Relative patch ID (control point ID included)
            entryArgIdxs.tcs.relPatchId = argIdx++;

            break;
        }
    case ShaderStageTessEval:
        {
            if (m_hasGs)    // TES acts as hardware ES
            {
                if (m_pContext->IsTessOffChip())
                {
                    entryArgIdxs.tes.offChipLdsBase = argIdx; // Off-chip LDS buffer base
                    argTys.push_back(m_pContext->Int32Ty());
                    *pInRegMask |= (1ull << (argIdx++));

                    argTys.push_back(m_pContext->Int32Ty());  // If is_offchip enabled
                    *pInRegMask |= (1ull << (argIdx++));
                }
                argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset
                entryArgIdxs.tes.esGsOffset = argIdx;
                *pInRegMask |= (1ull << (argIdx++));
            }
            else  // TES acts as hardware VS
            {
                if (m_pContext->IsTessOffChip() || enableXfb)
                {
                    entryArgIdxs.tes.streamOutData.streamInfo = argIdx;
                    argTys.push_back(m_pContext->Int32Ty());
                    *pInRegMask |= (1ull << (argIdx++));
                }

                if (enableXfb)
                {
                    argTys.push_back(m_pContext->Int32Ty()); // Stream-out info (ID, vertex count, enablement)
                    entryArgIdxs.vs.streamOutData.streamInfo = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));

                    argTys.push_back(m_pContext->Int32Ty()); // Stream-out write Index
                    entryArgIdxs.vs.streamOutData.writeIndex = argIdx;
                    *pInRegMask |= (1ull << (argIdx++));

                    for (uint32_t i = 0; i < MaxTransformFeedbackBuffers; ++i)
                    {
                        if (xfbStrides[i] > 0)
                        {
                            argTys.push_back(m_pContext->Int32Ty()); // Stream-out offset
                            entryArgIdxs.tes.streamOutData.streamOffsets[i] = argIdx;
                            *pInRegMask |= (1ull << (argIdx++));
                        }
                    }
                }

                if (m_pContext->IsTessOffChip()) // Off-chip LDS buffer base
                {
                    entryArgIdxs.tes.offChipLdsBase = argIdx;

                    argTys.push_back(m_pContext->Int32Ty());
                    *pInRegMask |= (1ull << (argIdx++));
                }
            }

            argTys.push_back(m_pContext->FloatTy()); // X of TessCoord (U)
            entryArgIdxs.tes.tessCoordX = argIdx++;

            argTys.push_back(m_pContext->FloatTy()); // Y of TessCoord (V)
            entryArgIdxs.tes.tessCoordY = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Relative patch ID
            entryArgIdxs.tes.relPatchId = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Patch ID
            entryArgIdxs.tes.patchId = argIdx++;

            break;
        }
    case ShaderStageGeometry:
        {
            argTys.push_back(m_pContext->Int32Ty()); // GS to VS offset
            entryArgIdxs.gs.gsVsOffset = argIdx;
            *pInRegMask |= (1ull << (argIdx++));

            argTys.push_back(m_pContext->Int32Ty()); // GS wave ID
            entryArgIdxs.gs.waveId = argIdx;
            *pInRegMask |= (1ull << (argIdx++));

            // TODO: We should make the arguments according to real usage.
            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 0)
            entryArgIdxs.gs.esGsOffsets[0] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 1)
            entryArgIdxs.gs.esGsOffsets[1] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Primitive ID
            entryArgIdxs.gs.primitiveId = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 2)
            entryArgIdxs.gs.esGsOffsets[2] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 3)
            entryArgIdxs.gs.esGsOffsets[3] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 4)
            entryArgIdxs.gs.esGsOffsets[4] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // ES to GS offset (vertex 5)
            entryArgIdxs.gs.esGsOffsets[5] = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Invocation ID
            entryArgIdxs.gs.invocationId = argIdx++;
            break;
        }
    case ShaderStageFragment:
        {
            argTys.push_back(m_pContext->Int32Ty()); // Primitive mask
            entryArgIdxs.fs.primMask = argIdx;
            *pInRegMask |= (1ull << (argIdx++));

            argTys.push_back(m_pContext->Floatx2Ty()); // Perspective sample
            entryArgIdxs.fs.perspInterp.sample = argIdx++;

            argTys.push_back(m_pContext->Floatx2Ty()); // Perspective center
            entryArgIdxs.fs.perspInterp.center = argIdx++;

            argTys.push_back(m_pContext->Floatx2Ty()); // Perspective centroid
            entryArgIdxs.fs.perspInterp.centroid = argIdx++;

            argTys.push_back(m_pContext->Floatx3Ty()); // Perspective pull-mode
            entryArgIdxs.fs.perspInterp.pullMode = argIdx++;

            argTys.push_back(m_pContext->Floatx2Ty()); // Linear sample
            entryArgIdxs.fs.linearInterp.sample = argIdx++;

            argTys.push_back(m_pContext->Floatx2Ty()); // Linear center
            entryArgIdxs.fs.linearInterp.center = argIdx++;

            argTys.push_back(m_pContext->Floatx2Ty()); // Linear centroid
            entryArgIdxs.fs.linearInterp.centroid = argIdx++;

            argTys.push_back(m_pContext->FloatTy()); // Line stipple
            ++argIdx;

            argTys.push_back(m_pContext->FloatTy()); // X of FragCoord
            entryArgIdxs.fs.fragCoord.x = argIdx++;

            argTys.push_back(m_pContext->FloatTy()); // Y of FragCoord
            entryArgIdxs.fs.fragCoord.y = argIdx++;

            argTys.push_back(m_pContext->FloatTy()); // Z of FragCoord
            entryArgIdxs.fs.fragCoord.z = argIdx++;

            argTys.push_back(m_pContext->FloatTy()); // W of FragCoord
            entryArgIdxs.fs.fragCoord.w = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Front facing
            entryArgIdxs.fs.frontFacing = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Ancillary
            entryArgIdxs.fs.ancillary = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Sample coverage
            entryArgIdxs.fs.sampleCoverage = argIdx++;

            argTys.push_back(m_pContext->Int32Ty()); // Fixed X/Y
            ++argIdx;

            break;
        }
    case ShaderStageCompute:
        {
            // Add system values in SGPR
            argTys.push_back(m_pContext->Int32x3Ty()); // WorkgroupId
            entryArgIdxs.cs.workgroupId = argIdx;
            *pInRegMask |= (1ull << (argIdx++));

            argTys.push_back(m_pContext->Int32Ty());  // Multiple dispatch info, include TG_SIZE and etc.
            *pInRegMask |= (1ull << (argIdx++));

            // Add system value in VGPR
            argTys.push_back(m_pContext->Int32x3Ty()); // LocalInvociationId
            entryArgIdxs.cs.localInvocationId = argIdx++;
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }

    return FunctionType::get(m_pContext->VoidTy(), argTys, false);
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for entry-point mutation.
INITIALIZE_PASS(PatchEntryPointMutate, DEBUG_TYPE,
                "Patch LLVM for entry-point mutation", false, false)
