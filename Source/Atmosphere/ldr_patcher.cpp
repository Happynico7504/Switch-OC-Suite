/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include "ldr_patcher.hpp"

namespace ams::ldr {

    namespace {

        constexpr const char *NsoPatchesDirectory = "exefs_patches";

        /* Exefs patches want to prevent modification of header, */
        /* and also want to adjust offset relative to mapped location. */
        constexpr size_t NsoPatchesProtectedSize   = sizeof(NsoHeader);
        constexpr size_t NsoPatchesProtectedOffset = sizeof(NsoHeader);

        constexpr const char * const LoaderSdMountName = "#amsldr-sdpatch";
        static_assert(sizeof(LoaderSdMountName) <= fs::MountNameLengthMax);

        constinit os::SdkMutex g_ldr_sd_lock;
        constinit bool g_mounted_sd;

        constinit os::SdkMutex g_embedded_patch_lock;
        constinit bool g_got_embedded_patch_settings;
        constinit bool g_force_enable_usb30;

        bool EnsureSdCardMounted() {
            std::scoped_lock lk(g_ldr_sd_lock);

            if (g_mounted_sd) {
                return true;
            }

            if (!cfg::IsSdCardInitialized()) {
                return false;
            }

            if (R_FAILED(fs::MountSdCard(LoaderSdMountName))) {
                return false;
            }

            return (g_mounted_sd = true);
        }

        bool IsUsb30ForceEnabled() {
            std::scoped_lock lk(g_embedded_patch_lock);

            if (!g_got_embedded_patch_settings) {
                g_force_enable_usb30 = spl::IsUsb30ForceEnabled();
                g_got_embedded_patch_settings = true;
            }

            return g_force_enable_usb30;
        }

        u32 GetEmcClock() {
            // RAM freqs to choose: 1600000, 1728000, 1795200, 1862400, 1894400, 1932800, 1996800, 2064000, 2099200, 2131200
            // RAM overclock could be UNSTABLE on some RAM without bumping up voltage,
            // and therefore show graphical glitches, hang randomly or even worse, corrupt your NAND
            return 1862400;
        }

        u32 GetCpuBoostClock() {
            return 1963500;
        }

        consteval u8 ParseNybble(char c) {
            AMS_ASSUME(('0' <= c && c <= '9') || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'));
            if ('0' <= c && c <= '9') {
                return c - '0' + 0x0;
            } else if ('A' <= c && c <= 'F') {
                return c - 'A' + 0xA;
            } else /* if ('a' <= c && c <= 'f') */ {
                return c - 'a' + 0xa;
            }
        }

        consteval ro::ModuleId ParseModuleId(const char *str) {
            /* Parse a static module id. */
            ro::ModuleId module_id = {};

            size_t ofs = 0;
            while (str[0] != 0) {
                AMS_ASSUME(ofs < sizeof(module_id));
                AMS_ASSUME(str[1] != 0);

                module_id.data[ofs] = (ParseNybble(str[0]) << 4) | (ParseNybble(str[1]) << 0);

                str += 2;
                ofs++;
            }

            return module_id;
        }

        struct EmbeddedPatchEntry {
            uintptr_t offset;
            const void * const data;
            size_t size;
        };

        struct EmbeddedPatch {
            ro::ModuleId module_id;
            size_t num_entries;
            const EmbeddedPatchEntry *entries;
        };

        #include "ldr_embedded_usb_patches.inc"

    }

    #include "ldr_oc_patch.inc"

    /* Apply IPS patches. */
    void LocateAndApplyIpsPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size) {
        if (!EnsureSdCardMounted()) {
            return;
        }

        ro::ModuleId module_id;
        std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));
        ams::patcher::LocateAndApplyIpsPatchesToModule(LoaderSdMountName, NsoPatchesDirectory, NsoPatchesProtectedSize, NsoPatchesProtectedOffset, std::addressof(module_id), reinterpret_cast<u8 *>(mapped_nso), mapped_size);
    }

    /* Apply embedded patches. */
    void ApplyEmbeddedPatchesToModule(const u8 *module_id_data, uintptr_t mapped_nso, size_t mapped_size) {
        /* Make module id. */
        ro::ModuleId module_id;
        std::memcpy(std::addressof(module_id.data), module_id_data, sizeof(module_id.data));

        if (IsUsb30ForceEnabled()) {
            for (const auto &patch : Usb30ForceEnablePatches) {
                if (std::memcmp(std::addressof(patch.module_id), std::addressof(module_id), sizeof(module_id)) == 0) {
                    for (size_t i = 0; i < patch.num_entries; ++i) {
                        const auto &entry = patch.entries[i];
                        if (entry.offset + entry.size <= mapped_size) {
                            std::memcpy(reinterpret_cast<void *>(mapped_nso + entry.offset), entry.data, entry.size);
                        }
                    }
                }
            }
        }

        u32 EmcClock = GetEmcClock();
        if (spl::GetSocType() == spl::SocType_Mariko && EmcClock) {
            for (u32 i = 0; i < sizeof(PcvModuleId)/sizeof(ro::ModuleId); i++) {
                if (std::memcmp(std::addressof(PcvModuleId[i]), std::addressof(module_id), sizeof(module_id)) == 0) {
                    /* Add new CPU and GPU clock tables for Mariko */
                    std::memcpy(reinterpret_cast<void *>(mapped_nso + CpuTablesFreeSpace[i]), NewCpuTables, sizeof(NewCpuTables));
                    std::memcpy(reinterpret_cast<void *>(mapped_nso + GpuTablesFreeSpace[i]), NewGpuTables, sizeof(NewGpuTables));

                    /* Patch Mariko max CPU and GPU clockrates */
                    std::memcpy(reinterpret_cast<void *>(mapped_nso + MaxCpuClockOffset[i]), &NewMaxCpuClock, sizeof(NewMaxCpuClock));
                    std::memcpy(reinterpret_cast<void *>(mapped_nso + Reg1MaxGpuOffset[i]), Reg1NewMaxGpuClock, sizeof(Reg1NewMaxGpuClock[i]));
                    std::memcpy(reinterpret_cast<void *>(mapped_nso + Reg2MaxGpuOffset[i]), Reg2NewMaxGpuClock, sizeof(Reg2NewMaxGpuClock[i]));

                    /* Patch max cpu voltage on Mariko */
                    for (u32 j = 0; j < sizeof(CpuVoltageLimitOffsets[i])/sizeof(u32); j++) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + CpuVoltageLimitOffsets[i][j]), &NewCpuVoltageLimit, sizeof(NewCpuVoltageLimit));
                    }
                    for (u32 j = 0; j < sizeof(CpuVoltageOldTableCoeff[i])/sizeof(u32); j++) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + CpuVoltageOldTableCoeff[i][j]), &NewCpuVoltageCoeff, sizeof(NewCpuVoltageCoeff));
                    }

                    /* Patch RAM Clock */
                    for (u32 j = 0; j < sizeof(EmcFreqOffsets[i])/sizeof(u32); j++) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + EmcFreqOffsets[i][j]), &EmcClock, sizeof(EmcClock));
                    }
                }
            }

            EmcClock = GetEmcClock() * 1000;

            u32 CpuBoostClock = GetCpuBoostClock() * 1000;

            for (u32 i = 0; i < sizeof(PtmModuleId)/sizeof(ro::ModuleId); i++) {
                if (std::memcmp(std::addressof(PtmModuleId[i]), std::addressof(module_id), sizeof(module_id)) == 0) {
                    for (u32 j = 0; j < 16; j++) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + PtmEmcOffsetStart[i] + PtmOffsetInterval * j), &EmcClock, sizeof(EmcClock));
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + PtmEmcOffsetStart[i] + PtmOffsetInterval * j + 0x4), &EmcClock, sizeof(EmcClock));
                    }
                    for (u32 j = 0; j < 2; j++) {
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + PtmEmcOffsetStart[i] + PtmCpuBoostOffset + PtmOffsetInterval * j), &CpuBoostClock, sizeof(CpuBoostClock));
                        std::memcpy(reinterpret_cast<void *>(mapped_nso + PtmEmcOffsetStart[i] + PtmCpuBoostOffset + PtmOffsetInterval * j + 0x4), &CpuBoostClock, sizeof(CpuBoostClock));
                    }
                }
            }
        }

        for (u32 i = 0; i < sizeof(AmModuleId)/sizeof(ro::ModuleId); i++) {
            if(std::memcmp(std::addressof(AmModuleId[i]), std::addressof(module_id), sizeof(module_id)) == 0) {
                std::memcpy(reinterpret_cast<void *>(mapped_nso + AmCopyrightOffset[i]), AmCopyrightPatch, sizeof(AmCopyrightPatch));
            }
        }
    }

}