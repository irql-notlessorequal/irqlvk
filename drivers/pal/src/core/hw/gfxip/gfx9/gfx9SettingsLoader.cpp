/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "pal.h"
#include "palInlineFuncs.h"
#include "palHashMapImpl.h"
#include "core/device.h"
#include "core/hw/gfxip/gfx9/gfx9Chip.h"
#include "core/hw/gfxip/gfx9/gfx9Device.h"
#include "core/hw/gfxip/gfx9/gfx9SettingsLoader.h"
#include "core/hw/gfxip/gfx9/gfx9Device.h"
#include "core/hw/gfxip/pm4CmdBuffer.h"
#include "core/hw/amdgpu_asic.h"
#include "devDriverServer.h"
#include "protocols/ddSettingsService.h"
#include "settingsService.h"
#if PAL_BUILD_GFX11
namespace Pal
{
#include "g_gfx11SwWarDetection.h"
}
#endif

using namespace Util;
using namespace Util::Literals;

namespace Pal
{
namespace Gfx9
{

// Minimum microcode feature version that has necessary MCBP fix.
constexpr uint32 MinUcodeFeatureVersionMcbpFix = 36;
#if PAL_BUILD_GFX11
// Minimum ucode version that supports the packed register pairs packet. Temporarily set to UINT_MAX to disable packet
// usage till additional testing and validation is completed.
constexpr uint32 MinPfpVersionPackedRegPairsPacket   = 1448;
// Minimum ucode version that supports the packed register pairs packet for compute. Currently not supported.
constexpr uint32 MinPfpVersionPackedRegPairsPacketCs = UINT_MAX;
// Minimum ucode version that supports the EVENT_WRITE_ZPASS packet.
constexpr uint32 MinPfpVersionEventWriteZpassPacket  = 1458;
#endif

// =====================================================================================================================
SettingsLoader::SettingsLoader(
    Pal::Device* pDevice)
    :
    Pal::ISettingsLoader(pDevice->GetPlatform(),
                         static_cast<DriverSettings*>(&m_settings),
                         g_gfx9PalNumSettings),
    m_pDevice(pDevice),
    m_settings{},
    m_gfxLevel(pDevice->ChipProperties().gfxLevel),
    m_pComponentName("Gfx9_Pal")
{

}

// =====================================================================================================================
SettingsLoader::~SettingsLoader()
{
    SettingsRpcService::SettingsService* pRpcSettingsService = m_pDevice->GetPlatform()->GetSettingsService();

    if (pRpcSettingsService != nullptr)
    {
        pRpcSettingsService->UnregisterComponent(m_pComponentName);
    }

    auto* pDevDriverServer = m_pDevice->GetPlatform()->GetDevDriverServer();
    if (pDevDriverServer != nullptr)
    {
        auto* pSettingsService = pDevDriverServer->GetSettingsService();
        if (pSettingsService != nullptr)
        {
            pSettingsService->UnregisterComponent(m_pComponentName);
        }
    }
}

// =====================================================================================================================
// Initializes the HWL environment settings.
Result SettingsLoader::Init()
{
    Result ret = m_settingsInfoMap.Init();

    if (ret == Result::Success)
    {
        // Init Settings Info HashMap
        InitSettingsInfo();

        // setup default values
        SetupDefaults();

        m_state = SettingsLoaderState::EarlyInit;

        // Read the rest of the settings from the registry
        ReadSettings();

        // Register with the DevDriver settings service
        DevDriverRegister();
    }

    return ret;
}

// =====================================================================================================================
// Validates that the settings structure has legal values. Variables that require complicated initialization can also
// be initialized here.
void SettingsLoader::ValidateSettings(
    PalSettings* pSettings)
{
    const auto& chipProps   = m_pDevice->ChipProperties();
    const auto& engineProps = m_pDevice->EngineProperties();
    const auto& gfx9Props   = chipProps.gfx9;

    auto* pPalSettings = m_pDevice->GetPublicSettings();

    if (IsGfx9(*m_pDevice))
    {
        // YUV planar surfaces require the ability to modify the base address to point to individual slices.  Due
        // to DCC addressing that interleaves slices on GFX9 platforms, we can't accurately point to the start
        // of a slice in DCC, which makes supporting DCC for YUV planar surfaces impossible.
        pSettings->useDcc &= ~UseDccYuvPlanar;
    }

    if (m_settings.binningMaxAllocCountLegacy == 0)
    {
        if (IsGfx9(*m_pDevice))
        {
            // The recommended value for MAX_ALLOC_COUNT is min(128, PC size in the number of cache lines/(2*2*NUM_SE)).
            // The first 2 is to account for the register doubling the value and second 2 is to allow for at least 2
            // batches to ping-pong.
            m_settings.binningMaxAllocCountLegacy =
                Min(128u, gfx9Props.parameterCacheLines / (4u * gfx9Props.numShaderEngines));
        }
        else if (IsGfx10(*m_pDevice))
        {
            // In Gfx10 there is a single view of the PC rather than a division per SE.
            // The recommended value for this is to allow a single batch to consume at
            // most 1/3 of the parameter cache lines.
            m_settings.binningMaxAllocCountLegacy = gfx9Props.parameterCacheLines / 3;
        }
    }

    if (m_settings.binningMaxAllocCountNggOnChip == 0)
    {
#if PAL_BUILD_GFX11
        if (IsGfx11(*m_pDevice))
        {
            // GFX11 eliminates the parameter cache, so the determination needs to come from elsewhere.
            // In addition, binningMaxAllocCountLegacy has no affect on GFX11.
            // The expectation is that a value of ~16, which is a workload size of 1024 prims, assuming 2 verts per
            // prim and 16 attribute groups (256 verts * 16 attr groups / 2 verts per prim / 2 screen tiles = 1024),
            // should be sufficient for building a batch. This value can be tuned further as more apps are running and
            // perf investigations continue to progress.
            m_settings.binningMaxAllocCountNggOnChip = 16;
        }
        else
#endif
        {
            // With NGG + on chip PC there is a single view of the PC rather than a
            // division per SE. The recommended value for this is to allow a single batch to
            // consume at most 1/3 of the parameter cache lines.
            // This applies to all of Gfx10, as the PC only has a single view for both legacy and NGG.
            m_settings.binningMaxAllocCountNggOnChip = gfx9Props.parameterCacheLines / 3;
        }

        if (IsGfx9(*m_pDevice))
        {
            // On GFX9, the PA_SC_BINNER_CNTL_1::MAX_ALLOC_COUNT value is in units of
            // 2 parameter cache lines. So divide by 2.
            m_settings.binningMaxAllocCountNggOnChip /= 2;
        }
    }

    // If a specific late-alloc GS value was requested in the panel, we want to supersede a client set value. This may
    // still be overridden to 0 below for sufficiently small GPUs.
    pPalSettings->nggLateAllocGs = m_settings.overrideNggLateAllocGs >= 0 ? m_settings.overrideNggLateAllocGs
                                                                          : pPalSettings->nggLateAllocGs;

    // Some hardware can support 128 offchip buffers per SE, but most support 64.
    const uint32 maxOffchipLdsBuffersPerSe = (gfx9Props.doubleOffchipLdsBuffers ? 128 : 64);

    // Compute the number of offchip LDS buffers for the whole chip.
    uint32 maxOffchipLdsBuffers = (gfx9Props.numShaderEngines * maxOffchipLdsBuffersPerSe);

    if (IsVega10(*m_pDevice))
    {
        // Vega10 has a HW bug where during Tessellation, the SPI can load incorrect SDATA terms for offchip LDS.
        // We must limit the number of offchip buffers to 508 (127 offchip buffers per SE).
        maxOffchipLdsBuffers = Min(maxOffchipLdsBuffers, 508U);
    }
#if PAL_BUILD_GFX11
    else if (IsGfx11(*m_pDevice))
    {
        // Gfx11 has more SEs than our previous products, and so the number of maxOffchipLdsBuffers is now a factor
        // of the number of SEs in the chip and there is no minimum.
        constexpr uint32 Gfx11MaxOffchipLdsBufferPerSe = 256;
        maxOffchipLdsBuffers = Gfx11MaxOffchipLdsBufferPerSe * gfx9Props.numShaderEngines;
    }
#endif
    else
    {
        // On gfx9, the offchip buffering register has enough space to support the full 512 buffers.
        maxOffchipLdsBuffers = Min(maxOffchipLdsBuffers, 512U);
    }

    // If the current microcode version doesn't support the "indexed" versions of the LOADDATA PM4 packets, we cannot
    // support MCBP because that feature requires using those packets.
    // We also need to make sure any microcode versions which are before the microcode fix disable preemption, even if
    // the user tried to enable it through the panel.
    if ((m_gfxLevel == GfxIpLevel::GfxIp9) &&
        (m_pDevice->ChipProperties().cpUcodeVersion < MinUcodeFeatureVersionMcbpFix))
    {
        // We don't have a fully correct path to enable in this case. The KMD needs us to respect their MCBP enablement
        // but we can't support state shadowing without these features.
        pSettings->cmdBufPreemptionMode = CmdBufPreemptModeFullDisableUnsafe;
    }
    else if (m_pDevice->GetPublicSettings()->disableCommandBufferPreemption)
    {
        pSettings->cmdBufPreemptionMode = CmdBufPreemptModeDisable;
    }

    // Validate the number of offchip LDS buffers used for tessellation.
    if (m_settings.numOffchipLdsBuffers > 0)
    {
        if (m_settings.useMaxOffchipLdsBuffers == true)
        {
            // Use the maximum amount of offchip-LDS buffers.
            m_settings.numOffchipLdsBuffers = maxOffchipLdsBuffers;
        }
        else
        {
            // Clamp to the maximum amount of offchip LDS buffers.
            m_settings.numOffchipLdsBuffers =
                    Min(maxOffchipLdsBuffers, m_settings.numOffchipLdsBuffers);
        }
    }

    // If HTile is disabled, also disable the other settings whic
    // If HTile is disabled, also disable the other settings which depend on it:
    if (m_settings.htileEnable == false)
    {
        m_settings.hiDepthEnable           = false;
        m_settings.hiStencilEnable         = false;
        m_settings.dbPreloadEnable         = false;
        m_settings.dbPreloadWinEnable      = false;
        m_settings.dbPerTileExpClearEnable = false;
        m_settings.depthCompressEnable     = false;
        m_settings.stencilCompressEnable   = false;
    }

    // This can't be enabled by default in PAL because enabling the feature requires doing an expand on any clear
    // that changes the depth/stencil clear value. In that case, tiles marked as EXPCLEAR no longer match the new clear
    // value. In PAL, we don't always have visibility into what the last clear value was(if the clear was done in a
    // different command buffer or thread), so we'd have to do the expand conditionally on the GPU which may have
    // Perf Implications. Hence, enable it only if client is sure about depth stencil surfaces never changing the
    // clear values which means PAL doesn't have to worry about any clear-time expand operation to remove
    // the exp-clear tiles.
    if (pPalSettings->hintInvariantDepthStencilClearValues)
    {
        m_settings.dbPerTileExpClearEnable = true;
    }

    pSettings->prefetchClampSize = Pow2Align(pSettings->prefetchClampSize, 4096);

    // By default, gfx9RbPlusEnable is true, and it should be overridden to false
    // if the ASIC doesn't support Rb+.
    if (gfx9Props.rbPlus == 0)
    {
        m_settings.gfx9RbPlusEnable = false;
        pPalSettings->optDepthOnlyExportRate = false;
    }

    if (gfx9Props.supportOutOfOrderPrimitives == 0)
    {
        m_settings.enableOutOfOrderPrimitives = OutOfOrderPrimDisable;
    }
    {
        if (pPalSettings->binningContextStatesPerBin == 0)
        {
            pPalSettings->binningContextStatesPerBin = 1;
        }
        if (pPalSettings->binningPersistentStatesPerBin == 0)
        {
            pPalSettings->binningPersistentStatesPerBin = 1;
        }
    }

    if (pPalSettings->disableBinningPsKill == OverrideMode::Default)
    {
        if (
            false)
        {
            pPalSettings->disableBinningPsKill = OverrideMode::Disabled;
        }
        else
        {
            pPalSettings->disableBinningPsKill = OverrideMode::Enabled;
        }
    }

    if (IsGfx10(*m_pDevice))
    {
        // GFX10 doesn't need this workaround as it can natively support 1D depth images.
        m_settings.treat1dAs2d = false;

        // GFX10 doesn't use the convoluted meta-addressing scheme that GFX9 does, so disable
        // the "optimized" algorithm for processing the meta-equations.
        m_settings.optimizedFastClear = 0;

        if ((m_settings.waLateAllocGs0) && m_settings.nggSupported)
        {
            pPalSettings->nggLateAllocGs = 0;

            // This workaround requires that tessellation distribution is enabled and the distribution factors are
            // non-zero.
            if (pPalSettings->distributionTessMode == DistributionTessOff)
            {
                pPalSettings->distributionTessMode = DistributionTessDefault;
            }
        }

        if (m_settings.gfx9RbPlusEnable)
        {
            m_settings.useCompToSingle |= (Gfx10UseCompToSingle8bpp | Gfx10UseCompToSingle16bpp);
        }
    }

    // On GFX103+ WGP harvesting asymmetric configuration, for pixel shader waves the extra WGP is not useful as all
    // of GFX103 splits workloads (waves) evenly among the SE. Using Navi2x as an example: For pixel shader workloads,
    // pixels are split evenly among the 2 SA within an SE as well. So for basic large uniform PS workload, pixels are
    // split evenly among all 8 SA of a Navi2x and the work-load will only finish as fast as the SA with the fewest # of
    // WGP. In essence this means that a 72 CU Navi21 behaves like a 64 CU Navi21 for pixel shader workloads.
    // We should mask off the extra WGP from PS waves on WGP harvesting asymmetric configuration.
    // This will reduce power consumption when not needed and allow to the GPU to clock higher.
    if (IsGfx103Plus(*m_pDevice) && m_settings.gfx103PlusDisableAsymmetricWgpForPs)
    {
        m_settings.psCuEnLimitMask = (1 << (gfx9Props.gfx10.minNumWgpPerSa * 2)) - 1;
    }

    uint32 tessFactRingSizeMask = Gfx09_10::VGT_TF_RING_SIZE__SIZE_MASK;
    uint32 tessFactScalar       = gfx9Props.numShaderEngines;

#if PAL_BUILD_GFX11
    if (IsGfx11(*m_pDevice))
    {
        // All GFX11 parts support RB+ which according to HW guarantees that
        // every 256 bytes line up on consistent pixel boundaries.
        m_settings.useCompToSingle |= (Gfx10UseCompToSingle8bpp | Gfx10UseCompToSingle16bpp);

        // Programming this on GFX11 is slightly different. The size has changed but more importantly the value is now
        // treated as per SE by the hardware so we don't need to scale by the number of SE's manually any more.
        tessFactRingSizeMask = Gfx11::VGT_TF_RING_SIZE__SIZE_MASK;
        tessFactScalar       = 1;
    }
#endif

    // Default values for Tess Factor buffer are safe. This could have been changed by the panel settings so for a
    // sanity check let's adjust the tess factor buffer size down if it's to big:
    if ((m_settings.tessFactorBufferSizePerSe * tessFactScalar) > tessFactRingSizeMask)
    {
        m_settings.tessFactorBufferSizePerSe = Pow2AlignDown(tessFactRingSizeMask, tessFactScalar) / tessFactScalar;
        static_assert(VGT_TF_RING_SIZE__SIZE__SHIFT == 0, "VGT_TF_RING_SIZE::SIZE shift is no longer zero!");
    }

#if PAL_BUILD_GFX11
    if (IsGfx11(*m_pDevice))
    {
        // GFX11 doesn't need this workaround as it can natively support 1D depth images.
        m_settings.treat1dAs2d = false;

        // GFX11 doesn't use the convoluted meta-addressing scheme that GFX9 does either, so disable
        // the "optimized" algorithm for processing the meta-equations.
        m_settings.optimizedFastClear = 0;

        // Vertex Attribute ring buffer must be aligned respect the maximum for the chip
        const uint32 maxAttribRingBufferSizePerSe =
            Pow2AlignDown(Gfx11VertexAttributeRingMaxSizeBytes / gfx9Props.numShaderEngines,
                          Gfx11VertexAttributeRingAlignmentBytes);

        m_settings.gfx11VertexAttributesRingBufferSizePerSe =
            Pow2Align(m_settings.gfx11VertexAttributesRingBufferSizePerSe, Gfx11VertexAttributeRingAlignmentBytes);

        m_settings.gfx11VertexAttributesRingBufferSizePerSe =
            Min(maxAttribRingBufferSizePerSe, m_settings.gfx11VertexAttributesRingBufferSizePerSe);

        if ((m_settings.waForceSpiThrottleModeNonZero) &&
            ((m_settings.defaultSpiGsThrottleCntl2 & Gfx11::SPI_GS_THROTTLE_CNTL2__SPI_THROTTLE_MODE_MASK) == 0))
        {
            // Force mode 1 (expected default) if we haven't already forced it via the setting.
            m_settings.defaultSpiGsThrottleCntl2 |= (1 << Gfx11::SPI_GS_THROTTLE_CNTL2__SPI_THROTTLE_MODE__SHIFT);
        }

        // GFX11 has different VRS programming than GFX10 and does not need this optimization.
        m_settings.optimizeNullSourceImage = false;

        // Clamp the sample-mask-tracker to the HW-legal values of 3-15.
#if (PAL_CLIENT_INTERFACE_MAJOR_VERSION < 777)
        pPalSettings->gfx11SampleMaskTrackerWatermark = ((pPalSettings->gfx11SampleMaskTrackerWatermark == 0) ?
            0 : Clamp(pPalSettings->gfx11SampleMaskTrackerWatermark, 3u, 15u));
#else
        m_settings.gfx11SampleMaskTrackerWatermark = ((m_settings.gfx11SampleMaskTrackerWatermark == 0) ?
            0 : Clamp(m_settings.gfx11SampleMaskTrackerWatermark, 3u, 15u));
#endif

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 818
        // Replace the "PublicSetting" enum with a final value. The rest of PAL can ignore Ac01WaPublicSetting.
        if (m_settings.waDisableAc01 == Ac01WaPublicSetting)
        {
            m_settings.waDisableAc01 = pPalSettings->ac01WaNotNeeded ? Ac01WaAllowAc01 : Ac01WaForbidAc01;
        }
#endif

    }
    else
    {
        m_settings.gfx11VertexAttributesRingBufferSizePerSe = 0;
    }
#endif

    if ((pPalSettings->distributionTessMode == DistributionTessTrapezoidOnly) ||
        (pPalSettings->distributionTessMode == DistributionTessDefault))
    {
        pPalSettings->distributionTessMode = DistributionTessTrapezoid;
    }

    // When WD load balancing flowchart optimization is enabled, the primgroup size cannot exceed 253.
    m_settings.primGroupSize = Min(253u, m_settings.primGroupSize);

    if (chipProps.gfxLevel == GfxIpLevel::GfxIp9)
    {
        m_settings.nggSupported = false;
    }

    // Set default value for DCC BPP Threshold unless it was already overriden
    if (pPalSettings->dccBitsPerPixelThreshold == UINT_MAX)
    {
        // Performance testing on Vega20 has shown that it generally performs better when it's restricted
        // to use DCC at >=64BPP, we thus set it's default DCC threshold to 64BPP unless otherwise overriden.
        if (IsVega20(*m_pDevice))
        {
            pPalSettings->dccBitsPerPixelThreshold = 64;
        }
        else
        {
            pPalSettings->dccBitsPerPixelThreshold = 0;
        }
    }

    // For sufficiently small GPUs, we want to disable late-alloc and allow NGG waves access to the whole chip.
    if (IsGfx10Plus(chipProps.gfxLevel) &&
        ((chipProps.gfx9.gfx10.minNumWgpPerSa <= 2) || (chipProps.gfx9.numActiveCus < 4)))
    {
        constexpr uint32 MaskEnableAll                          = UINT_MAX;
        m_settings.gsCuEnLimitMask                              = MaskEnableAll;
        m_settings.allowNggOnAllCusWgps                         = true;
        pPalSettings->nggLateAllocGs                            = 0;
#if PAL_BUILD_GFX11
        // Gfx11 has attributes through memory, so parameter cache space is not a concern and we can continue to
        // enable LateAlloc for the parameter cache.
        if (IsGfx11(chipProps.gfxLevel) == false)
#endif
        {
            m_settings.gfx10GePcAllocNumLinesPerSeLegacyNggPassthru = 0;
            m_settings.gfx10GePcAllocNumLinesPerSeNggCulling        = 0;
        }
    }

    // nggLateAllocGs can NOT be greater than 127.
    pPalSettings->nggLateAllocGs = Min(pPalSettings->nggLateAllocGs, 127u);

#if PAL_BUILD_NAVI33
    constexpr uint32 Navi33GopherUltraLite = 0x74;

    if ((chipProps.revision == AsicRevision::Navi33) && (chipProps.deviceId == Navi33GopherUltraLite))
    {
        // Navi33 Ultra Lite Model has 1 SEs, 2 SAs/SE, 1 WGP/SA
        // In this model if a CU is reserved for PS firestrike hangs
        constexpr uint32 MaskEnableAll  = UINT_MAX;
        m_settings.gsCuEnLimitMask      = MaskEnableAll;
        m_settings.allowNggOnAllCusWgps = true;
    }
#endif

    // Since XGMI is much faster than PCIe, PAL should not reduce the number of RBs to increase the PCIe throughput
    if (chipProps.p2pSupport.xgmiEnabled != 0)
    {
        pSettings->nonlocalDestGraphicsCopyRbs = UINT_MAX;
    }

    m_state = SettingsLoaderState::Final;
}

// =====================================================================================================================
// Setup any workarounds that are necessary for all Gfx10 products.
static void SetupGfx10Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    pSettings->waColorCacheControllerInvalidEviction = true;

    // GCR ranged sync operations cause page faults for Cmask without the uCode fix that properly converts the
    // ACQUIRE_MEM packet's COHER_SIZE to the correct GCR_DATA_INDEX.
    pSettings->waCmaskImageSyncs = (device.ChipProperties().cpUcodeVersion < 28);

    // We can't use CP_PERFMON_STATE_STOP_COUNTING when using an SQ counters or they can get stuck off until we reboot.
    pSettings->waNeverStopSqCounters = true;
}

// =====================================================================================================================
// Setup workarounds that are necessary for all Gfx10.1 products.
static void SetupGfx101Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings,
    PalSettings*        pCoreSettings)
{
    pSettings->waVgtFlushNggToLegacyGs = true;

    pSettings->waVgtFlushNggToLegacy = true;

    pSettings->waDisableFmaskNofetchOpOnFmaskCompressionDisable = true;

    // The GE has a bug where attempting to use an index buffer of size zero can cause a hang.
    // The workaround is to bind an internal index buffer of a single entry and force the index buffer
    // size to one. This applies to all Navi1x products, which are all Gfx10.1 products.
    pSettings->waIndexBufferZeroSize = true;

    // The CB has a bug where blending can be corrupted if the color target is 8bpp and uses an S swizzle mode.
    pCoreSettings->addr2DisableSModes8BppColor = true;

    pSettings->waCeDisableIb2 = true;

    pSettings->waUtcL0InconsistentBigPage = true;

    pSettings->waLimitLateAllocGsNggFifo = true;

    pSettings->waClampGeCntlVertGrpSize = true;

    pSettings->waLegacyGsCutModeFlush = true;

    {
        // The DB has a bug where an attempted depth expand of a Z16_UNORM 1xAA surface that has not had its
        // metadata initialized will cause the DBs to incorrectly calculate the amount of return data from the
        // RMI block, which results in a hang.
        // The workaround is to force a compute resummarize for these surfaces, as we can't guarantee that an
        // expand won't be executed on an uninitialized depth surface.
        // This applies to all Navi1x products, which are all Gfx10.1 products.
        pSettings->waZ16Unorm1xAaDecompressUninitialized = true;

        // Workaround gfx10 Ngg performance issues related to UTCL2 misses with Index Buffers.
        pSettings->waEnableIndexBufferPrefetchForNgg = true;

        // Applies to all Navi1x products.
        pSettings->waClampQuadDistributionFactor = true;

        pSettings->waLogicOpDisablesOverwriteCombiner = true;

        // Applies to all Navi1x products.
        // If Primitive Order Pixel Shader (POPS/ROVs) are enabled and DB_DFSM_CONTROL.POPS_DRAIN_PS_ON_OVERLAP == 1,
        // we must set DB_RENDER_OVERRIDE2.PARTIAL_SQUAD_LAUNCH_CONTROL = PSLC_ON_HANG_ONLY to avoid a hang.
        pSettings->waStalledPopsMode = true;

        // The DB has a bug that when setting the iterate_256 register to 1 causes a hang.
        // More specifically the Flush Sequencer state-machine gets stuck waiting for Z data
        // when Iter256 is set to 1. The software workaround is to set DECOMPRESS_ON_N_ZPLANES
        // register to 2 for 4x MSAA Depth/Stencil surfaces to prevent hangs.
        pSettings->waTwoPlanesIterate256 = true;
    }
}

// =====================================================================================================================
// Setup workarounds that are necessary for all Navi2x products.
static void SetupNavi2xWorkarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    // This bug is caused by shader UAV writes to stencil surfaces that have associated hTile data that in turn
    // contains VRS data.  The UAV to stencil will corrupt the VRS data.  No API that supports VRS allows for
    // application writes to stencil UAVs; however, PAL does it internally through image-to-image copies.  Force
    // use of graphics copies for affected surfaces.
    pSettings->waVrsStencilUav = WaVrsStencilUav::GraphicsCopies;

    pSettings->waLegacyGsCutModeFlush = true;

    // When instance packing is enabled for adjacent prim_types and num_instances >1, pipeline stats generated by GE
    // are incorrect.
    // This workaround is to disable instance packing for adjacent prim_types.
    pSettings->waDisableInstancePacking = true;

    // On Navi2x hw, the polarity of AutoFlushMode is inverted, thus setting this value to true as a Navi2x workaround.
    // The AUTO_FLUSH_MODE flag will be properly inverted as a part of PerfExperiment.
    pSettings->waAutoFlushModePolarityInversed = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi10.
static void SetupNavi10Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings,
    PalSettings*        pCoreSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Gfx10.1 workarounds.
    SetupGfx101Workarounds(device, pSettings, pCoreSettings);

    // Setup any Navi10 specific workarounds.

    pSettings->waSdmaPreventCompressedSurfUse = true;

    pSettings->waFixPostZConservativeRasterization = true;

    pSettings->waTessIncorrectRelativeIndex = true;

    pSettings->waForceZonlyHtileForMipmaps = true;
} // PAL_BUILD_NAVI10

// =====================================================================================================================
// Setup workarounds that only apply to Navi14.
static void SetupNavi14Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings,
    PalSettings*        pCoreSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Gfx10.1 workarounds.
    SetupGfx101Workarounds(device, pSettings, pCoreSettings);

    // Setup any Navi14 specific workarounds.

    pSettings->waLateAllocGs0 = true;
    pSettings->nggSupported   = false;
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi12.
static void SetupNavi12Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings,
    PalSettings*        pCoreSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Gfx10.1 workarounds.
    SetupGfx101Workarounds(device, pSettings, pCoreSettings);

    // Setup any Navi12 workarounds.
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi21.
static void SetupNavi21Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Navi21 workarounds.

    pSettings->waCeDisableIb2 = true;

    pSettings->waDisableFmaskNofetchOpOnFmaskCompressionDisable = true;

    pSettings->waVgtFlushNggToLegacy = true;

    pSettings->waDisableVrsWithDsExports = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi22.
static void SetupNavi22Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Navi22 workarounds.

    pSettings->waCeDisableIb2 = true;

    pSettings->waDisableVrsWithDsExports = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi23.
static void SetupNavi23Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Navi23 workarounds.
    pSettings->waBadSqttFinishResults = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Navi23.
static void SetupNavi24Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Navi24 workarounds.
    pSettings->waBadSqttFinishResults = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Rembrandt.
static void SetupRembrandtWorkarounds(
    const Pal::Device& device,
    Gfx9PalSettings*   pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Rembrandt workarounds.
    pSettings->waBadSqttFinishResults = true;
}

// =====================================================================================================================
// Setup workarounds that only apply to Raphael.
static void SetupRaphaelWorkarounds(
    const Pal::Device& device,
    Gfx9PalSettings*   pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Raphael workarounds.
}

// =====================================================================================================================
// Setup workarounds that only apply to Mendocino.
static void SetupMendocinoWorkarounds(
    const Pal::Device& device,
    Gfx9PalSettings* pSettings)
{
    // Setup any Gfx10 workarounds.
    SetupGfx10Workarounds(device, pSettings);

    // Setup any Navi2x workarounds.
    SetupNavi2xWorkarounds(device, pSettings);

    // Setup any Mendocino workarounds.
}

#if PAL_BUILD_GFX11
// =====================================================================================================================
// Setup workarounds for GFX11
static void SetupGfx11Workarounds(
    const Pal::Device&  device,
    Gfx9PalSettings*    pSettings)
{
    const uint32 familyId = device.ChipProperties().familyId;
    const uint32 eRevId   = device.ChipProperties().eRevId;

    Gfx11SwWarDetection workarounds = {};
    const bool waFound = DetectGfx11SoftwareWorkaroundsByChip(familyId, eRevId, &workarounds);
    PAL_ASSERT(waFound);

#if PAL_ENABLE_PRINTS_ASSERTS
    constexpr uint32 HandledWaMask[] = { 0x1E793001, 0x00004B00 }; // Workarounds handled by PAL.
    constexpr uint32 OutsideWaMask[] = { 0xE0068DFE, 0x000014FC }; // Workarounds handled by other components.
    constexpr uint32 MissingWaMask[] = { 0x00004000, 0x00002001 }; // Workarounds that should be handled by PAL that
                                                                   // are not yet implemented or are unlikey to be
                                                                   // implemented.
    constexpr uint32 InvalidWaMask[] = { 0x01800200, 0x00002002 }; // Workarounds marked invalid, thus not handled.
    static_assert((sizeof(HandledWaMask) == sizeof(Gfx11InactiveMask)) &&
                  (sizeof(OutsideWaMask) == sizeof(Gfx11InactiveMask)) &&
                  (sizeof(MissingWaMask) == sizeof(Gfx11InactiveMask)) &&
                  (sizeof(InvalidWaMask) == sizeof(Gfx11InactiveMask)),
                  "Workaround Masks do not match expected size!");

    constexpr uint32 InactiveMask[] = { ~(HandledWaMask[0] | OutsideWaMask[0] | MissingWaMask[0] | InvalidWaMask[0] ),
                                        ~(HandledWaMask[1] | OutsideWaMask[1] | MissingWaMask[1] | InvalidWaMask[1] ) };
    static_assert((InactiveMask[0] == Gfx11InactiveMask[0]) && (InactiveMask[1] == Gfx11InactiveMask[1]),
                  "Workaround Masks do not match!");
#endif

    static_assert(Gfx11NumWorkarounds == 47, "Workaround count mismatch between PAL and SWD");

#if PAL_BUILD_NAVI31
    if (workarounds.ppPbbPBBBreakBatchDifferenceWithPrimLimit_FpovLimit_DeallocLimit_A_)
    {
        if (pSettings->binningFpovsPerBatch == 0)
        {
            pSettings->binningFpovsPerBatch = 255;
        }

        if (pSettings->binningMaxAllocCountNggOnChip == 0)
        {
            pSettings->binningMaxAllocCountNggOnChip = 255;
        }
    }
#endif

#if PAL_BUILD_NAVI33
    pSettings->waForceSpiThrottleModeNonZero =
        workarounds.sioSpiBciSpyGlassRevealedABugInSpiRaRscselGsThrottleModuleWhichIsCausedByGsPsVgprLdsInUsesVariableDroppingMSBInRelevantMathExpression_A_;
#endif

#if PAL_BUILD_NAVI3X
    pSettings->waReplaceEventsWithTsEvents = workarounds.ppDbPWSIssueForDepthWrite_TextureRead_A_;

    pSettings->waAddPostambleEvent = workarounds.geometryGeGEWdTe11ClockCanStayHighAfterShaderMessageThdgrp_A_;
#endif

#if PAL_BUILD_GFX11
    pSettings->waLineStippleReset             = workarounds.geometryPaPALineStippleResetError_A_;

    // These two settings are different ways of solving the same problem.  We've experimentally
    // determined that "intrinsic rate enable" has better performance.
    pSettings->gfx11DisableRbPlusWithBlending = false;
    pSettings->waEnableIntrinsicRateEnable    = workarounds.sioSpiBciSPI_TheOverRestrictedExportConflictHQ_HoldingQueue_PtrRuleMayReduceTheTheoreticalExpGrantThroughput_PotentiallyIncreaseOldNewPSWavesInterleavingChances_A_;
#endif

    // This workaround requires disabling use of the AC01 clear codees, which is what the "force regular clear code"
    // panel setting is already designed to do.
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 818
    if (workarounds.ppCbGFX11DCC31DXXPNeedForSpeedHeat_BlackFlickeringDotCorruption_A_ != 0)
    {
        // Note that the public setting defaults to "enable the workaround".
        pSettings->waDisableAc01 = Ac01WaPublicSetting;
    }
#endif

    pSettings->waSqgTtWptrOffsetFixup = workarounds.shaderSqSqgSQGTTWPTRIssues_A_;

    pSettings->waCbPerfCounterStuckZero =
        workarounds.gcPvPpCbCBPerfcountersStuckAtZeroAfterPerfcounterStopEventReceived_A_;

    pSettings->waForcePrePixShaderWaitPoint =
        workarounds.ppDbPWS_RtlTimeout_TimeStampEventPwsStall_eopDoneNotSentForOldestTSWaitingForSyncComplete__FlusherStalledInOpPipe_A_;

    pSettings->waForceLockThresholdZero = workarounds.sioSpiBciSoftLockIssue_A_;

    pSettings->waSetVsXyNanToInfZero = workarounds.geometryPaStereoPositionNanCheckBug_A_;

    pSettings->waIncorrectMaxAllowedTilesInWave = workarounds.ppDbPpScSCDBHangNotSendingWaveConflictBackToSPI_A_;

    if (false
#if PAL_BUILD_NAVI31
        || IsNavi31(device)
#endif
#if PAL_BUILD_NAVI32
        || IsNavi32(device)
#endif
        )
    {
        pSettings->shaderPrefetchSizeBytes = 0;
    }

#if PAL_BUILD_GFX11
#if (PAL_CLIENT_INTERFACE_MAJOR_VERSION < 777)
    if (device.GetPublicSettings()->gfx11SampleMaskTrackerWatermark > 0)
#else
    if (pSettings->gfx11SampleMaskTrackerWatermark > 0)
#endif
    {
        pSettings->waitOnFlush |= (WaitAfterCbFlush | WaitBeforeBarrierEopWithCbFlush);
    }
#endif

}
#endif

// =====================================================================================================================
// Override Gfx9 layer settings. This also includes setting up the workaround flags stored in the settings structure
// based on chip Family & ID.
//
// The workaround flags setup here can be overridden if the settings are set.
void SettingsLoader::OverrideDefaults(
    PalSettings* pSettings)
{
    const Pal::Device& device          = *m_pDevice;
    PalPublicSettings* pPublicSettings = m_pDevice->GetPublicSettings();

    uint16 minBatchBinSizeWidth  = 128;
    uint16 minBatchBinSizeHeight = 64;

    // Enable workarounds which are common to all Gfx9 hardware.
    if (IsGfx9(device))
    {
        m_settings.nggSupported = false;

        m_settings.waColorCacheControllerInvalidEviction = true;

        m_settings.waDisableHtilePrefetch = true;

        m_settings.waOverwriteCombinerTargetMaskOnly = true;

        m_settings.waDummyZpassDoneBeforeTs = true;

        m_settings.waLogicOpDisablesOverwriteCombiner = true;

        // Metadata is not pipe aligned once we get down to the mip chain within the tail
        m_settings.waitOnMetadataMipTail = true;

        // Set this to 1 in Gfx9 to enable CU soft group for PS by default. VS soft group is turned off by default.
        m_settings.numPsWavesSoftGroupedPerCu = 1;

        m_settings.waDisableSCompressSOnly = true;

        if (IsVega10(device) || IsRaven(device))
        {
            m_settings.waHtilePipeBankXorMustBeZero = true;

            m_settings.waWrite1xAASampleLocationsToZero = true;

            m_settings.waMiscPopsMissedOverlap = true;

            m_settings.waMiscScissorRegisterChange = true;

            m_settings.waDisable24BitHWFormatForTCCompatibleDepth = true;
        }

        if (device.ChipProperties().gfx9.rbPlus != 0)
        {
            m_settings.waRotatedSwizzleDisablesOverwriteCombiner = true;
        }

        if (IsVega10(device) || IsRaven(device) || IsRaven2(device) || IsRenoir(device))
        {
            m_settings.waMetaAliasingFixEnabled = false;
        }
    }
    else if (IsGfx10(device))
    {
        if (IsNavi10(device))
        {
            SetupNavi10Workarounds(device, &m_settings, pSettings);
        }
        else if (IsNavi14(device))
        {
            SetupNavi14Workarounds(device, &m_settings, pSettings);
        }
        else if (IsNavi12(device))
        {
            SetupNavi12Workarounds(device, &m_settings, pSettings);
        }
        else if (IsNavi21(device))
        {
            SetupNavi21Workarounds(device, &m_settings);
        }
        else if (IsNavi22(device))
        {
            SetupNavi22Workarounds(device, &m_settings);
        }
        else if (IsNavi23(device))
        {
            SetupNavi23Workarounds(device, &m_settings);
        }
        else if (IsNavi24(device))
        {
            SetupNavi24Workarounds(device, &m_settings);
        }
        else if (IsRembrandt(device))
        {
            SetupRembrandtWorkarounds(device, &m_settings);
        }
        else if (IsRaphael(device))
        {
            SetupRaphaelWorkarounds(device, &m_settings);
        }
        else if (IsMendocino(device))
        {
            SetupMendocinoWorkarounds(device, &m_settings);
        }

    }
#if PAL_BUILD_GFX11
    else if (IsGfx11(device))
    {
        SetupGfx11Workarounds(device, &m_settings);

        m_settings.numTsMsDrawEntriesPerSe = 1024;

        // GFX11 supports modifying the group size.  Use the maximum setting.
        m_settings.ldsPsGroupSize = Gfx10LdsPsGroupSize::Gfx10LdsPsGroupSizeDouble;

        // GFX11 doesn't have HW support for DB -> CB copies.
        m_settings.allowDepthCopyResolve = false;

        m_settings.defaultSpiGsThrottleCntl1 = Gfx11::mmSPI_GS_THROTTLE_CNTL1_DEFAULT;
        m_settings.defaultSpiGsThrottleCntl2 = Gfx11::mmSPI_GS_THROTTLE_CNTL2_DEFAULT;

        // Recommended by HW that LATE_ALLOC_GS be set to 63 on GFX11
        pPublicSettings->nggLateAllocGs = 63;

        // Apply this to all Gfx11 APUs
        if (device.ChipProperties().gpuType == GpuType::Integrated)
        {
            // APU tuning with 2MB L2 Cache shows ATM Ring Buffer size 768 KiB yields best performance
            m_settings.gfx11VertexAttributesRingBufferSizePerSe = 768_KiB;

        }
    }
#endif

    if (IsGfx103Plus(device))
    {
        m_settings.gfx103PlusDisableAsymmetricWgpForPs = true;
    }

#if PAL_BUILD_GFX11
    const uint32 pfpUcodeVersion = m_pDevice->ChipProperties().pfpUcodeVersion;

    m_settings.gfx11EnableContextRegPairOptimization = pfpUcodeVersion >= MinPfpVersionPackedRegPairsPacket;
    m_settings.gfx11EnableShRegPairOptimization      = pfpUcodeVersion >= MinPfpVersionPackedRegPairsPacket;
    m_settings.gfx11EnableShRegPairOptimizationCs    = pfpUcodeVersion >= MinPfpVersionPackedRegPairsPacketCs;

    m_settings.gfx11EnableZpassPacketOptimization    = pfpUcodeVersion >= MinPfpVersionEventWriteZpassPacket;
#endif

    // If minimum sizes are 0, then use default size.
    if (m_settings.minBatchBinSize.width == 0)
    {
        m_settings.minBatchBinSize.width = minBatchBinSizeWidth;
    }
    if (m_settings.minBatchBinSize.height == 0)
    {
        m_settings.minBatchBinSize.height = minBatchBinSizeHeight;
    }

    m_state = SettingsLoaderState::LateInit;
}

// =====================================================================================================================
// The settings hash is used during pipeline loading to verify that the pipeline data is compatible between when it was
// stored and when it was loaded.
void SettingsLoader::GenerateSettingHash()
{
    MetroHash128::Hash(
        reinterpret_cast<const uint8*>(&m_settings),
        sizeof(Gfx9PalSettings),
        m_settingHash.bytes);
}

} // Gfx9
} // Pal
