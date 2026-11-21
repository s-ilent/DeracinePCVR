// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <vector>
#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "common/types.h"
#include "core/memory.h"
#include "core/signals.h"
#include "core/tls.h"
#include "cpu_patches.h"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#else
#if defined(__FreeBSD__)
#include <machine/npx.h>
#endif
#include <pthread.h>
#include <sys/ucontext.h>
#endif

using namespace Xbyak::util;

namespace Core {

static Xbyak::Reg ZydisToXbyakRegister(const ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D) {
        return Xbyak::Reg32(reg - ZYDIS_REGISTER_EAX + Xbyak::Operand::EAX);
    }
    if (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15) {
        return Xbyak::Reg64(reg - ZYDIS_REGISTER_RAX + Xbyak::Operand::RAX);
    }
    if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM31) {
        return Xbyak::Xmm(reg - ZYDIS_REGISTER_XMM0 + xmm0.getIdx());
    }
    if (reg >= ZYDIS_REGISTER_YMM0 && reg <= ZYDIS_REGISTER_YMM31) {
        return Xbyak::Ymm(reg - ZYDIS_REGISTER_YMM0 + ymm0.getIdx());
    }
    UNREACHABLE_MSG("Unsupported register: {}", static_cast<u32>(reg));
}

static Xbyak::Reg ZydisToXbyakRegisterOperand(const ZydisDecodedOperand& operand) {
    ASSERT_MSG(operand.type == ZYDIS_OPERAND_TYPE_REGISTER,
               "Expected register operand, got type: {}", static_cast<u32>(operand.type));

    return ZydisToXbyakRegister(operand.reg.value);
}

static Xbyak::Address ZydisToXbyakMemoryOperand(const ZydisDecodedOperand& operand) {
    ASSERT_MSG(operand.type == ZYDIS_OPERAND_TYPE_MEMORY, "Expected memory operand, got type: {}",
               static_cast<u32>(operand.type));

    if (operand.mem.base == ZYDIS_REGISTER_RIP) {
        return ptr[rip + operand.mem.disp.value];
    }

    Xbyak::RegExp expression{};
    if (operand.mem.base != ZYDIS_REGISTER_NONE) {
        expression = expression + ZydisToXbyakRegister(operand.mem.base);
    }
    if (operand.mem.index != ZYDIS_REGISTER_NONE) {
        if (operand.mem.scale != 0) {
            expression = expression + ZydisToXbyakRegister(operand.mem.index) * operand.mem.scale;
        } else {
            expression = expression + ZydisToXbyakRegister(operand.mem.index);
        }
    }
    if (operand.mem.disp.size != 0 && operand.mem.disp.value != 0) {
        expression = expression + operand.mem.disp.value;
    }

    return ptr[expression];
}

static bool FilterTcbAccess(const ZydisDecodedOperand* operands) {
    const auto& dst_op = operands[0];
    const auto& src_op = operands[1];

    // Patch only 'mov (64-bit register), fs:[0]'
    return src_op.type == ZYDIS_OPERAND_TYPE_MEMORY && src_op.mem.segment == ZYDIS_REGISTER_FS &&
           src_op.mem.base == ZYDIS_REGISTER_NONE && src_op.mem.index == ZYDIS_REGISTER_NONE &&
           src_op.mem.disp.value == 0 && dst_op.reg.value >= ZYDIS_REGISTER_RAX &&
           dst_op.reg.value <= ZYDIS_REGISTER_R15;
}

static void GenerateTcbAccess(void* /* address */, const ZydisDecodedOperand* operands,
                              Xbyak::CodeGenerator& c) {
    const auto dst = ZydisToXbyakRegisterOperand(operands[0]);

#if defined(_WIN32)
    // The following logic is based on the Kernel32.dll asm of TlsGetValue
    static constexpr u32 TlsSlotsOffset = 0x1480;
    static constexpr u32 TlsExpansionSlotsOffset = 0x1780;
    static constexpr u32 TlsMinimumAvailable = 64;

    const auto slot = GetTcbKey();

    // Load the pointer to the table of TLS slots.
    c.putSeg(gs);
    if (slot < TlsMinimumAvailable) {
        // Load the pointer to TLS slots.
        c.mov(dst, ptr[reinterpret_cast<void*>(TlsSlotsOffset + slot * sizeof(LPVOID))]);
    } else {
        const u32 tls_index = slot - TlsMinimumAvailable;

        // Load the pointer to the table of TLS expansion slots.
        c.mov(dst, ptr[reinterpret_cast<void*>(TlsExpansionSlotsOffset)]);
        // Load the pointer to our buffer.
        c.mov(dst, qword[dst + tls_index * sizeof(LPVOID)]);
    }
#else
    const auto src = ZydisToXbyakMemoryOperand(operands[1]);

    // Replace fs read with gs read.
    c.putSeg(gs);
    c.mov(dst, src);
#endif
}

static bool FilterStackCheck(const ZydisDecodedOperand* operands) {
    const auto& dst_op = operands[0];
    const auto& src_op = operands[1];

    // Some compilers emit stack checks by starting a function with
    // 'mov (64-bit register), fs:[0x28]', then checking with `xor (64-bit register), fs:[0x28]`
    return src_op.type == ZYDIS_OPERAND_TYPE_MEMORY && src_op.mem.segment == ZYDIS_REGISTER_FS &&
           src_op.mem.base == ZYDIS_REGISTER_NONE && src_op.mem.index == ZYDIS_REGISTER_NONE &&
           src_op.mem.disp.value == 0x28 && dst_op.reg.value >= ZYDIS_REGISTER_RAX &&
           dst_op.reg.value <= ZYDIS_REGISTER_R15;
}

static void GenerateStackCheck(void* /* address */, const ZydisDecodedOperand* operands,
                               Xbyak::CodeGenerator& c) {
    const auto dst = ZydisToXbyakRegisterOperand(operands[0]);
    c.xor_(dst, 0);
}

static void GenerateStackCanary(void* /* address */, const ZydisDecodedOperand* operands,
                                Xbyak::CodeGenerator& c) {
    const auto dst = ZydisToXbyakRegisterOperand(operands[0]);
    c.mov(dst, 0);
}

static bool FilterNoSSE4a(const ZydisDecodedOperand*) {
    Cpu cpu;
    return !cpu.has(Cpu::tSSE4a);
}

static void GenerateEXTRQ(void* /* address */, const ZydisDecodedOperand* operands,
                          Xbyak::CodeGenerator& c) {
    bool immediateForm = operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                         operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;

    ASSERT_MSG(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER, "operand 0 must be a register");

    const auto dst = ZydisToXbyakRegisterOperand(operands[0]);

    ASSERT_MSG(dst.isXMM(), "operand 0 must be an XMM register");

    Xbyak::Xmm xmm_dst = *reinterpret_cast<const Xbyak::Xmm*>(&dst);

    if (immediateForm) {
        u8 length = operands[1].imm.value.u & 0x3F;
        u8 index = operands[2].imm.value.u & 0x3F;

        LOG_DEBUG(Core, "Patching immediate form EXTRQ, length: {}, index: {}", length, index);

        const Xbyak::Reg64 scratch1 = rax;
        const Xbyak::Reg64 scratch2 = rcx;

        // Set rsp to before red zone and save scratch registers
        c.lea(rsp, ptr[rsp - 128]);
        c.pushfq();
        c.push(scratch1);
        c.push(scratch2);

        u64 mask;
        if (length == 0) {
            length = 64; // for the check below
            mask = 0xFFFF'FFFF'FFFF'FFFF;
        } else {
            mask = (1ULL << length) - 1;
        }

        if (length + index > 64) {
            mask = 0xFFFF'FFFF'FFFF'FFFF;
        }

        // Get lower qword from xmm register
        c.vmovq(scratch1, xmm_dst);

        if (index != 0) {
            c.shr(scratch1, index);
        }

        // We need to move mask to a register because we can't use all the possible
        // immediate values with `and reg, imm32`
        c.mov(scratch2, mask);
        c.and_(scratch1, scratch2);

        // Writeback to xmm register, extrq instruction says top 64-bits are undefined but zeroed on
        // AMD CPUs
        c.vmovq(xmm_dst, scratch1);

        c.pop(scratch2);
        c.pop(scratch1);
        c.popfq();
        c.lea(rsp, ptr[rsp + 128]);
    } else {
        ASSERT_MSG(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                       operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                       operands[0].reg.value >= ZYDIS_REGISTER_XMM0 &&
                       operands[0].reg.value <= ZYDIS_REGISTER_XMM15 &&
                       operands[1].reg.value >= ZYDIS_REGISTER_XMM0 &&
                       operands[1].reg.value <= ZYDIS_REGISTER_XMM15,
                   "Unexpected operand types for EXTRQ instruction");

        const auto src = ZydisToXbyakRegisterOperand(operands[1]);

        ASSERT_MSG(src.isXMM(), "operand 1 must be an XMM register");

        Xbyak::Xmm xmm_src = *reinterpret_cast<const Xbyak::Xmm*>(&src);

        const Xbyak::Reg64 scratch1 = rax;
        const Xbyak::Reg64 scratch2 = rcx;
        const Xbyak::Reg64 mask = rdx;

        Xbyak::Label length_zero, end;

        c.lea(rsp, ptr[rsp - 128]);
        c.pushfq();
        c.push(scratch1);
        c.push(scratch2);
        c.push(mask);

        // Construct the mask out of the length that resides in bottom 6 bits of source xmm
        c.vmovq(scratch1, xmm_src);
        c.mov(scratch2, scratch1);
        c.and_(scratch2, 0x3F);
        c.jz(length_zero);

        // mask = (1ULL << length) - 1
        c.mov(mask, 1);
        c.shl(mask, cl);
        c.dec(mask);
        c.jmp(end);

        c.L(length_zero);
        c.mov(mask, 0xFFFF'FFFF'FFFF'FFFF);

        c.L(end);

        // Get the shift amount and store it in scratch2
        c.shr(scratch1, 8);
        c.and_(scratch1, 0x3F);
        c.mov(scratch2, scratch1); // cl now contains the shift amount

        c.vmovq(scratch1, xmm_dst);
        c.shr(scratch1, cl);
        c.and_(scratch1, mask);
        c.vmovq(xmm_dst, scratch1);

        c.pop(mask);
        c.pop(scratch2);
        c.pop(scratch1);
        c.popfq();
        c.lea(rsp, ptr[rsp + 128]);
    }
}

static void GenerateINSERTQ(void* /* address */, const ZydisDecodedOperand* operands,
                            Xbyak::CodeGenerator& c) {
    bool immediateForm = operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                         operands[3].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;

    ASSERT_MSG(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                   operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER,
               "operands 0 and 1 must be registers.");

    const auto dst = ZydisToXbyakRegisterOperand(operands[0]);
    const auto src = ZydisToXbyakRegisterOperand(operands[1]);

    ASSERT_MSG(dst.isXMM() && src.isXMM(), "operands 0 and 1 must be xmm registers.");

    Xbyak::Xmm xmm_dst = *reinterpret_cast<const Xbyak::Xmm*>(&dst);
    Xbyak::Xmm xmm_src = *reinterpret_cast<const Xbyak::Xmm*>(&src);

    if (immediateForm) {
        u8 length = operands[2].imm.value.u & 0x3F;
        u8 index = operands[3].imm.value.u & 0x3F;

        const Xbyak::Reg64 scratch1 = rax;
        const Xbyak::Reg64 scratch2 = rcx;
        const Xbyak::Reg64 mask = rdx;

        // Set rsp to before red zone and save scratch registers
        c.lea(rsp, ptr[rsp - 128]);
        c.pushfq();
        c.push(scratch1);
        c.push(scratch2);
        c.push(mask);

        u64 mask_value;
        if (length == 0) {
            length = 64; // for the check below
            mask_value = 0xFFFF'FFFF'FFFF'FFFF;
        } else {
            mask_value = (1ULL << length) - 1;
        }

        if (length + index > 64) {
            mask_value = 0xFFFF'FFFF'FFFF'FFFF;
        }

        c.vmovq(scratch1, xmm_src);
        c.vmovq(scratch2, xmm_dst);
        c.mov(mask, mask_value);

        // src &= mask
        c.and_(scratch1, mask);

        // src <<= index
        c.shl(scratch1, index);

        // dst &= ~(mask << index)
        mask_value = ~(mask_value << index);
        c.mov(mask, mask_value);
        c.and_(scratch2, mask);

        // dst |= src
        c.or_(scratch2, scratch1);

        // Insert scratch2 into low 64 bits of dst, upper 64 bits are undefined but zeroed on AMD
        // CPUs
        c.vmovq(xmm_dst, scratch2);

        c.pop(mask);
        c.pop(scratch2);
        c.pop(scratch1);
        c.popfq();
        c.lea(rsp, ptr[rsp + 128]);
    } else {
        ASSERT_MSG(operands[2].type == ZYDIS_OPERAND_TYPE_UNUSED &&
                       operands[3].type == ZYDIS_OPERAND_TYPE_UNUSED,
                   "operands 2 and 3 must be unused for register form.");

        const Xbyak::Reg64 scratch1 = rax;
        const Xbyak::Reg64 scratch2 = rcx;
        const Xbyak::Reg64 index = rdx;
        const Xbyak::Reg64 mask = rbx;

        Xbyak::Label length_zero, end;

        c.lea(rsp, ptr[rsp - 128]);
        c.pushfq();
        c.push(scratch1);
        c.push(scratch2);
        c.push(index);
        c.push(mask);

        // Get upper 64 bits of src and copy it to mask and index
        c.vpextrq(index, xmm_src, 1);
        c.mov(mask, index);

        // When length is 0, set it to 64
        c.and_(mask, 0x3F); // mask now holds the length
        c.jz(length_zero);  // Check if length is 0 and set mask to all 1s if it is

        // Create a mask out of the length
        c.mov(cl, mask.cvt8());
        c.mov(mask, 1);
        c.shl(mask, cl);
        c.dec(mask);
        c.jmp(end);

        c.L(length_zero);
        c.mov(mask, 0xFFFF'FFFF'FFFF'FFFF);

        c.L(end);
        // Get index to insert at
        c.shr(index, 8);
        c.and_(index, 0x3F);

        // src &= mask
        c.vmovq(scratch1, xmm_src);
        c.and_(scratch1, mask);

        // mask = ~(mask << index)
        c.mov(cl, index.cvt8());
        c.shl(mask, cl);
        c.not_(mask);

        // src <<= index
        c.shl(scratch1, cl);

        // dst = (dst & mask) | src
        c.vmovq(scratch2, xmm_dst);
        c.and_(scratch2, mask);
        c.or_(scratch2, scratch1);

        // Upper 64 bits are undefined in insertq but AMD CPUs zero them
        c.vmovq(xmm_dst, scratch2);

        c.pop(mask);
        c.pop(index);
        c.pop(scratch2);
        c.pop(scratch1);
        c.popfq();
        c.lea(rsp, ptr[rsp + 128]);
    }
}

static void ReplaceMOVNT(void* address, u8 rep_prefix) {
    // Find the opcode byte
    // There can be any amount of prefixes but the instruction can't be more than 15 bytes
    // And we know for sure this is a MOVNTSS/MOVNTSD
    bool found = false;
    bool rep_prefix_found = false;
    int index = 0;
    u8* ptr = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 15; i++) {
        if (ptr[i] == rep_prefix) {
            rep_prefix_found = true;
        } else if (ptr[i] == 0x2B) {
            index = i;
            found = true;
            break;
        }
    }

    // Some sanity checks
    ASSERT(found);
    ASSERT(index >= 2);
    ASSERT(ptr[index - 1] == 0x0F);
    ASSERT(rep_prefix_found);

    // This turns the MOVNTSS/MOVNTSD to a MOVSS/MOVSD m, xmm
    ptr[index] = 0x11;
}

static void ReplaceMOVNTSS(void* address, const ZydisDecodedOperand*, Xbyak::CodeGenerator&) {
    ReplaceMOVNT(address, 0xF3);
}

static void ReplaceMOVNTSD(void* address, const ZydisDecodedOperand*, Xbyak::CodeGenerator&) {
    ReplaceMOVNT(address, 0xF2);
}

using PatchFilter = bool (*)(const ZydisDecodedOperand*);
using InstructionGenerator = void (*)(void*, const ZydisDecodedOperand*, Xbyak::CodeGenerator&);
struct PatchInfo {
    /// Filter for more granular patch conditions past just the instruction mnemonic.
    PatchFilter filter;

    /// Generator for the patch/trampoline.
    InstructionGenerator generator;

    /// Whether to use a trampoline for this patch.
    bool trampoline;
};

static const std::unordered_map<ZydisMnemonic, std::vector<PatchInfo>> Patches = {
    // SSE4a
    {ZYDIS_MNEMONIC_EXTRQ, {{FilterNoSSE4a, GenerateEXTRQ, true}}},
    {ZYDIS_MNEMONIC_INSERTQ, {{FilterNoSSE4a, GenerateINSERTQ, true}}},
    {ZYDIS_MNEMONIC_MOVNTSS, {{FilterNoSSE4a, ReplaceMOVNTSS, false}}},
    {ZYDIS_MNEMONIC_MOVNTSD, {{FilterNoSSE4a, ReplaceMOVNTSD, false}}},

#if !defined(__APPLE__)
    // FS segment patches
    // These first two patches are for accesses to the stack canary, fs:[0x28]
    {ZYDIS_MNEMONIC_XOR, {{FilterStackCheck, GenerateStackCheck, false}}},
    {ZYDIS_MNEMONIC_MOV,
     {{FilterStackCheck, GenerateStackCanary, false},
#if defined(_WIN32)
      // Windows needs a trampoline for Tcb accesses.
      {FilterTcbAccess, GenerateTcbAccess, true}
#else
      {FilterTcbAccess, GenerateTcbAccess, false}
#endif
     }},
#endif
};

static std::once_flag init_flag;

struct PatchModule {
    /// Mutex controlling access to module code regions.
    std::mutex mutex{};

    /// Start of the module.
    u8* start;

    /// End of the module.
    u8* end;

    /// Tracker for patched code locations.
    std::set<u8*> patched;

    /// Code generator for patching the module.
    Xbyak::CodeGenerator patch_gen;

    /// Code generator for writing trampoline patches.
    Xbyak::CodeGenerator trampoline_gen;

    /// Bounds of the guest executable trampoline area associated with this module.
    u8* trampoline_start;
    u8* trampoline_end;

    PatchModule(u8* module_ptr, const u64 module_size, u8* trampoline_ptr,
                const u64 trampoline_size)
        : start(module_ptr), end(module_ptr + module_size), patch_gen(module_size, module_ptr),
          trampoline_gen(trampoline_size, trampoline_ptr),
          trampoline_start(static_cast<u8*>(trampoline_ptr)),
          trampoline_end(static_cast<u8*>(trampoline_ptr) + trampoline_size) {}
};
static std::map<u64, PatchModule> modules;

static PatchModule* GetModule(const void* ptr) {
    auto upper_bound = modules.upper_bound(reinterpret_cast<u64>(ptr));
    if (upper_bound == modules.begin()) {
        return nullptr;
    }
    return &(std::prev(upper_bound)->second);
}

/// Returns a boolean indicating whether the instruction was patched, and the offset to advance past
/// whatever is at the current code pointer.
static std::pair<bool, u64> TryPatch(u8* code, PatchModule* module) {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status = Common::Decoder::Instance()->decodeInstruction(instruction, operands, code,
                                                                       module->end - code);
    if (!ZYAN_SUCCESS(status)) {
        return std::make_pair(false, 1);
    }

    if (Patches.contains(instruction.mnemonic)) {
        const auto& patches = Patches.at(instruction.mnemonic);
        for (const auto& patch_info : patches) {
            bool needs_trampoline = patch_info.trampoline;
            if (patch_info.filter(operands)) {
                auto& patch_gen = module->patch_gen;

                if (needs_trampoline && instruction.length < 5) {
                    // Trampoline is needed but instruction is too short to patch.
                    // Return false and length to signal to AOT compilation that this instruction
                    // should be skipped and handled at runtime.
                    return std::make_pair(false, instruction.length);
                }

                // Reset state and move to current code position.
                patch_gen.reset();
                patch_gen.setSize(code - patch_gen.getCode());

                if (needs_trampoline) {
                    auto& trampoline_gen = module->trampoline_gen;
                    const auto trampoline_ptr = trampoline_gen.getCurr();

                    patch_info.generator(code, operands, trampoline_gen);

                    // Return to the following instruction at the end of the trampoline.
                    trampoline_gen.jmp(code + instruction.length);

                    // Replace instruction with near jump to the trampoline.
                    patch_gen.jmp(trampoline_ptr, Xbyak::CodeGenerator::LabelType::T_NEAR);
                } else {
                    patch_info.generator(code, operands, patch_gen);
                }

                const auto patch_size = patch_gen.getCurr() - code;
                if (patch_size > 0) {
                    ASSERT_MSG(instruction.length >= patch_size,
                               "Instruction {} with length {} is too short to replace at: {}",
                               ZydisMnemonicGetString(instruction.mnemonic), instruction.length,
                               fmt::ptr(code));

                    // Fill remaining space with nops.
                    patch_gen.nop(instruction.length - patch_size);

                    module->patched.insert(code);
                    LOG_DEBUG(Core, "Patched instruction '{}' at: {}",
                              ZydisMnemonicGetString(instruction.mnemonic), fmt::ptr(code));
                    return std::make_pair(true, instruction.length);
                }
            }
        }
    }

    return std::make_pair(false, instruction.length);
}

static bool Is4ByteExtrqOrInsertq(void* code_address) {
    u8* bytes = (u8*)code_address;
    if (bytes[0] == 0x66 && bytes[1] == 0x0F && bytes[2] == 0x79) {
        return true; // extrq
    } else if (bytes[0] == 0xF2 && bytes[1] == 0x0F && bytes[2] == 0x79) {
        return true; // insertq
    } else {
        return false;
    }
}

static void* GetXmmPointer(void* ctx, u8 index) {
#if defined(_WIN32)
#define CASE(index)                                                                                \
    case index:                                                                                    \
        return (void*)(&((EXCEPTION_POINTERS*)ctx)->ContextRecord->Xmm##index.Low)
#elif defined(__APPLE__)
#define CASE(index)                                                                                \
    case index:                                                                                    \
        return (void*)(&((ucontext_t*)ctx)->uc_mcontext->__fs.__fpu_xmm##index);
#elif defined(__FreeBSD__)
    // In mc_fpstate
    // See <machine/npx.h> for the internals of mc_fpstate[].
#define CASE(index)                                                                                \
    case index: {                                                                                  \
        auto& mctx = ((ucontext_t*)ctx)->uc_mcontext;                                              \
        ASSERT(mctx.mc_fpformat == _MC_FPFMT_XMM);                                                 \
        auto* s_fpu = (struct savefpu*)(&mctx.mc_fpstate[0]);                                      \
        return (void*)(&(s_fpu->sv_xmm[0]));                                                       \
    }
#else
#define CASE(index)                                                                                \
    case index:                                                                                    \
        return (void*)(&((ucontext_t*)ctx)->uc_mcontext.fpregs->_xmm[index].element[0])
#endif
    switch (index) {
        CASE(0);
        CASE(1);
        CASE(2);
        CASE(3);
        CASE(4);
        CASE(5);
        CASE(6);
        CASE(7);
        CASE(8);
        CASE(9);
        CASE(10);
        CASE(11);
        CASE(12);
        CASE(13);
        CASE(14);
        CASE(15);
    default: {
        UNREACHABLE_MSG("Invalid XMM register index: {}", index);
        return nullptr;
    }
    }
#undef CASE
}

static void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip += length;
#else
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] += length;
#endif
}

static bool TryExecuteIllegalInstruction(void* ctx, void* code_address) {
    // We need to decode the instruction to find out what it is. Normally we'd use a fully fleshed
    // out decoder like Zydis, however Zydis does a bunch of stuff that impact performance that we
    // don't care about. We can get information about the instruction a lot faster by writing a mini
    // decoder here, since we know it is definitely an extrq or an insertq. If for some reason we
    // need to interpret more instructions in the future (I don't see why we would), we can revert
    // to using Zydis.
    ZydisMnemonic mnemonic;
    u8* bytes = (u8*)code_address;
    if (bytes[0] == 0x66) {
        mnemonic = ZYDIS_MNEMONIC_EXTRQ;
    } else if (bytes[0] == 0xF2) {
        mnemonic = ZYDIS_MNEMONIC_INSERTQ;
    } else {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status =
            Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
        LOG_ERROR(Core, "Unhandled illegal instruction at code address {}: {}",
                  fmt::ptr(code_address),
                  ZYAN_SUCCESS(status) ? ZydisMnemonicGetString(instruction.mnemonic)
                                       : "Failed to decode");
        return false;
    }

    ASSERT(bytes[1] == 0x0F && bytes[2] == 0x79);

    // Note: It's guaranteed that there's no REX prefix in these instructions checked by
    // Is4ByteExtrqOrInsertq
    u8 modrm = bytes[3];
    u8 rm = modrm & 0b111;
    u8 reg = (modrm >> 3) & 0b111;
    u8 mod = (modrm >> 6) & 0b11;

    ASSERT(mod == 0b11); // Any instruction we interpret here uses reg/reg addressing only

    int dstIndex = reg;
    int srcIndex = rm;

    switch (mnemonic) {
    case ZYDIS_MNEMONIC_EXTRQ: {
        const auto dst = GetXmmPointer(ctx, dstIndex);
        const auto src = GetXmmPointer(ctx, srcIndex);

        u64 lowQWordSrc;
        memcpy(&lowQWordSrc, src, sizeof(lowQWordSrc));

        u64 lowQWordDst;
        memcpy(&lowQWordDst, dst, sizeof(lowQWordDst));

        u64 length = lowQWordSrc & 0x3F;
        u64 mask;
        if (length == 0) {
            length = 64; // for the check below
            mask = 0xFFFF'FFFF'FFFF'FFFF;
        } else {
            mask = (1ULL << length) - 1;
        }

        u64 index = (lowQWordSrc >> 8) & 0x3F;
        if (length + index > 64) {
            // Undefined behavior if length + index is bigger than 64 according to the spec,
            // we'll warn and continue execution.
            LOG_TRACE(Core,
                      "extrq at {} with length {} and index {} is bigger than 64, "
                      "undefined behavior",
                      fmt::ptr(code_address), length, index);
        }

        lowQWordDst >>= index;
        lowQWordDst &= mask;

        memset((u8*)dst + sizeof(u64), 0, sizeof(u64));
        memcpy(dst, &lowQWordDst, sizeof(lowQWordDst));

        IncrementRip(ctx, 4);

        return true;
    }
    case ZYDIS_MNEMONIC_INSERTQ: {
        const auto dst = GetXmmPointer(ctx, dstIndex);
        const auto src = GetXmmPointer(ctx, srcIndex);

        u64 lowQWordSrc, highQWordSrc;
        memcpy(&lowQWordSrc, src, sizeof(lowQWordSrc));
        memcpy(&highQWordSrc, (u8*)src + 8, sizeof(highQWordSrc));

        u64 lowQWordDst;
        memcpy(&lowQWordDst, dst, sizeof(lowQWordDst));

        u64 length = highQWordSrc & 0x3F;
        u64 mask;
        if (length == 0) {
            length = 64; // for the check below
            mask = 0xFFFF'FFFF'FFFF'FFFF;
        } else {
            mask = (1ULL << length) - 1;
        }

        u64 index = (highQWordSrc >> 8) & 0x3F;
        if (length + index > 64) {
            // Undefined behavior if length + index is bigger than 64 according to the spec,
            // we'll warn and continue execution.
            LOG_TRACE(Core,
                      "insertq at {} with length {} and index {} is bigger than 64, "
                      "undefined behavior",
                      fmt::ptr(code_address), length, index);
        }

        lowQWordSrc &= mask;
        lowQWordDst &= ~(mask << index);
        lowQWordDst |= lowQWordSrc << index;

        memset((u8*)dst + sizeof(u64), 0, sizeof(u64));
        memcpy(dst, &lowQWordDst, sizeof(lowQWordDst));

        IncrementRip(ctx, 4);

        return true;
    }
    default: {
        UNREACHABLE();
    }
    }

    UNREACHABLE();
}

static bool TryPatchJit(void* code_address) {
    auto* code = static_cast<u8*>(code_address);
    auto* module = GetModule(code);
    if (module == nullptr) {
        return false;
    }

    std::unique_lock lock{module->mutex};

    // Return early if already patched, in case multiple threads signaled at the same time.
    if (std::ranges::find(module->patched, code) != module->patched.end()) {
        return true;
    }

    return TryPatch(code, module).first;
}

static void TryPatchAot(void* code_address, u64 code_size) {
    auto* code = static_cast<u8*>(code_address);
    auto* module = GetModule(code);
    if (module == nullptr) {
        return;
    }

    std::unique_lock lock{module->mutex};

    const auto* end = code + code_size;
    while (code < end) {
        code += TryPatch(code, module).second;
    }
}

static bool PatchesAccessViolationHandler(void* context, void* /* fault_address */) {
    return TryPatchJit(Common::GetRip(context));
}

static bool PatchesIllegalInstructionHandler(void* context) {
    void* code_address = Common::GetRip(context);
    if (Is4ByteExtrqOrInsertq(code_address)) {
        // The instruction is not big enough for a relative jump, don't try to patch it and pass it
        // to our illegal instruction interpreter directly
        return TryExecuteIllegalInstruction(context, code_address);
    } else {
        if (!TryPatchJit(code_address)) {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            const auto status =
                Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
            if (ZYAN_SUCCESS(status)) {
                const auto disassembled = Common::Decoder::Instance()->disassembleInst(
                    instruction, operands, std::bit_cast<u64>(code_address));
                LOG_ERROR(Core, "Failed to patch address {:x} -- mnemonic: {}, instruction: {}",
                          reinterpret_cast<u64>(code_address),
                          ZydisMnemonicGetString(instruction.mnemonic), disassembled);
            } else {
                LOG_ERROR(Core, "Failed to patch address {:x} -- mnemonic: (failed to decode)",
                          reinterpret_cast<u64>(code_address));
            }
            return false;
        }
    }

    return true;
}

static void PatchesInit() {
    if (!Patches.empty()) {
        auto* signals = Signals::Instance();
        // Should be called last.
        constexpr auto priority = std::numeric_limits<u32>::max();
        signals->RegisterAccessViolationHandler(PatchesAccessViolationHandler, priority);
        signals->RegisterIllegalInstructionHandler(PatchesIllegalInstructionHandler, priority);
    }
}

namespace {

constexpr size_t GuestEntryRedZoneSize = 128;
constexpr size_t GuestEntryStackAdjustment =
    sizeof(GuestEntryRegisterFrame) + GuestEntryRedZoneSize;
static_assert(GuestEntryStackAdjustment == 776,
              "The full YMM frame plus guest red zone must remain 776 bytes");
static_assert(GuestEntryStackAdjustment % 16 == 8,
              "The saved entry frame must align the observer call stack");

[[nodiscard]] static bool AddSignedAddress(VAddr base, s64 offset, VAddr& result) {
    if (offset >= 0) {
        const auto amount = static_cast<u64>(offset);
        if (base > (std::numeric_limits<VAddr>::max)() - amount)
            return false;
        result = base + amount;
        return true;
    }
    const auto amount = static_cast<u64>(-(offset + 1)) + 1;
    if (base < amount)
        return false;
    result = base - amount;
    return true;
}

[[nodiscard]] static bool RelativeDisplacement(VAddr target, VAddr next_instruction,
                                               s64& displacement) {
    if (target >= next_instruction) {
        const auto delta = target - next_instruction;
        if (delta > static_cast<u64>((std::numeric_limits<s64>::max)()))
            return false;
        displacement = static_cast<s64>(delta);
        return true;
    }
    const auto delta = next_instruction - target;
    const auto min_magnitude = static_cast<u64>((std::numeric_limits<s64>::max)()) + 1;
    if (delta > min_magnitude)
        return false;
    displacement =
        delta == min_magnitude ? (std::numeric_limits<s64>::min)() : -static_cast<s64>(delta);
    return true;
}

[[nodiscard]] static bool WriteSignedDisplacement(u8* bytes, size_t instruction_length,
                                                  u8 field_offset, u8 field_size_bits,
                                                  s64 displacement) {
    if (field_size_bits != 8 && field_size_bits != 16 && field_size_bits != 32)
        return false;
    const size_t field_size = field_size_bits / 8;
    if (field_offset > instruction_length || field_size > instruction_length - field_offset) {
        return false;
    }
    const s64 min_value = -(1LL << (field_size_bits - 1));
    const s64 max_value = (1LL << (field_size_bits - 1)) - 1;
    if (displacement < min_value || displacement > max_value)
        return false;
    const auto encoded = static_cast<u64>(displacement);
    for (size_t i = 0; i < field_size; ++i) {
        bytes[field_offset + i] = static_cast<u8>(encoded >> (i * 8));
    }
    return true;
}

[[nodiscard]] static bool RemapTrampolineTarget(VAddr target, VAddr original_start,
                                                VAddr trampoline_start, size_t stolen_length,
                                                VAddr& remapped) {
    if (original_start > (std::numeric_limits<VAddr>::max)() - stolen_length ||
        trampoline_start > (std::numeric_limits<VAddr>::max)() - stolen_length) {
        return false;
    }
    const VAddr original_end = original_start + stolen_length;
    if (target < original_start || target > original_end) {
        remapped = target;
        return true;
    }
    remapped = trampoline_start + (target - original_start);
    return true;
}

[[nodiscard]] static bool RelocateEntryInstruction(const u8* source_bytes, VAddr source_address,
                                                   u8* destination_bytes, VAddr destination_address,
                                                   size_t instruction_length, VAddr original_start,
                                                   VAddr trampoline_start, size_t stolen_length) {
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            instruction, operands, const_cast<u8*>(source_bytes), instruction_length)) ||
        instruction.length != instruction_length) {
        return false;
    }
    std::memcpy(destination_bytes, source_bytes, instruction_length);

    bool wrote_displacement = false;
    for (u8 i = 0; i < instruction.operand_count; ++i) {
        const auto& operand = operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP) {
            if (wrote_displacement || instruction.raw.disp.size == 0)
                return false;
            VAddr referenced_address{};
            VAddr next_source{};
            VAddr next_destination{};
            if (source_address > (std::numeric_limits<VAddr>::max)() - instruction_length ||
                destination_address > (std::numeric_limits<VAddr>::max)() - instruction_length) {
                return false;
            }
            next_source = source_address + instruction_length;
            next_destination = destination_address + instruction_length;
            if (!AddSignedAddress(next_source, operand.mem.disp.value, referenced_address) ||
                original_start > (std::numeric_limits<VAddr>::max)() - stolen_length) {
                return false;
            }
            const VAddr stolen_end = original_start + stolen_length;
            // A RIP-relative data reference into the overwritten bytes would observe the jump
            // patch instead of the original bytes; fail closed rather than redirecting it into
            // the trampoline's callback stub.
            if (referenced_address >= original_start && referenced_address < stolen_end) {
                return false;
            }
            s64 new_displacement{};
            if (!RelativeDisplacement(referenced_address, next_destination, new_displacement) ||
                !WriteSignedDisplacement(destination_bytes, instruction_length,
                                         instruction.raw.disp.offset, instruction.raw.disp.size,
                                         new_displacement)) {
                return false;
            }
            wrote_displacement = true;
        } else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
            if (wrote_displacement || instruction.raw.imm[0].size == 0)
                return false;
            VAddr branch_target{};
            VAddr next_source{};
            VAddr next_destination{};
            if (source_address > (std::numeric_limits<VAddr>::max)() - instruction_length ||
                destination_address > (std::numeric_limits<VAddr>::max)() - instruction_length) {
                return false;
            }
            next_source = source_address + instruction_length;
            next_destination = destination_address + instruction_length;
            if (!AddSignedAddress(next_source, operand.imm.value.s, branch_target) ||
                !RemapTrampolineTarget(branch_target, original_start, trampoline_start,
                                       stolen_length, branch_target)) {
                return false;
            }
            s64 new_displacement{};
            if (!RelativeDisplacement(branch_target, next_destination, new_displacement) ||
                !WriteSignedDisplacement(destination_bytes, instruction_length,
                                         instruction.raw.imm[0].offset, instruction.raw.imm[0].size,
                                         new_displacement)) {
                return false;
            }
            wrote_displacement = true;
        }
    }
    return true;
}

[[nodiscard]] static bool FlushCodeRange(void* address, size_t size) {
    if (address == nullptr || size == 0)
        return false;
    std::atomic_thread_fence(std::memory_order_seq_cst);
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(), address, size))
        return false;
    int registers[4]{};
    __cpuid(registers, 0);
    (void)registers[0];
#else
    auto* begin = static_cast<char*>(address);
    __builtin___clear_cache(begin, begin + size);
    unsigned int eax = 0;
    unsigned int ebx{};
    unsigned int ecx = 0;
    unsigned int edx{};
    asm volatile("mfence\n\tcpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx) : : "memory");
    (void)eax;
    (void)ebx;
    (void)ecx;
    (void)edx;
#endif
    return true;
}

[[nodiscard]] static bool GenerateGuestEntryTrampoline(PatchModule& module, VAddr target,
                                                       VAddr mapped_end,
                                                       GuestEntryObserver observer, void* user_data,
                                                       EntryPatchPlan& patch,
                                                       VAddr& trampoline_entry) {
    auto* target_bytes = reinterpret_cast<u8*>(target);
    const VAddr module_start = reinterpret_cast<VAddr>(module.start);
    const VAddr safe_end = (std::min)(reinterpret_cast<VAddr>(module.end), mapped_end);
    if (target < module_start || target >= safe_end)
        return false;
    const auto available = static_cast<size_t>(safe_end - target);
    std::array<u8, EntryPatchPlan::MaxBytes> original{};
    std::array<u8, EntryPatchPlan::MaxBytes> instruction_lengths{};
    size_t stolen_length = 0;
    size_t instruction_count = 0;
    while (stolen_length < 5) {
        if (stolen_length >= original.size() || stolen_length >= available)
            return false;
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const auto bytes_available = (std::min<size_t>)(15, available - stolen_length);
        if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                instruction, operands, target_bytes + stolen_length, bytes_available)) ||
            instruction.length == 0 || instruction.length > 15 ||
            instruction.length > original.size() - stolen_length) {
            return false;
        }
        std::memcpy(original.data() + stolen_length, target_bytes + stolen_length,
                    instruction.length);
        instruction_lengths[instruction_count++] = instruction.length;
        stolen_length += instruction.length;
    }

    auto& generator = module.trampoline_gen;
    auto* const generated_begin = generator.getCurr();
    trampoline_entry = reinterpret_cast<VAddr>(generated_begin);
    if (reinterpret_cast<VAddr>(generated_begin) <
            reinterpret_cast<VAddr>(module.trampoline_start) ||
        reinterpret_cast<VAddr>(generated_begin) >=
            reinterpret_cast<VAddr>(module.trampoline_end)) {
        return false;
    }
    const auto plan = BuildEntryPatchPlan(
        target, trampoline_entry, std::span<const u8>{original.data(), stolen_length},
        std::span<const u8>{instruction_lengths.data(), instruction_count});
    if (!plan)
        return false;

    try {
        auto& c = generator;
        c.lea(rsp, ptr[rsp - GuestEntryStackAdjustment]);
#define SAVE_GPR(member, reg) c.mov(qword[rsp + offsetof(GuestEntryRegisterFrame, member)], reg)
        SAVE_GPR(r15, r15);
        SAVE_GPR(r14, r14);
        SAVE_GPR(r13, r13);
        SAVE_GPR(r12, r12);
        SAVE_GPR(r11, r11);
        SAVE_GPR(r10, r10);
        SAVE_GPR(r9, r9);
        SAVE_GPR(r8, r8);
        SAVE_GPR(rdi, rdi);
        SAVE_GPR(rsi, rsi);
        SAVE_GPR(rbp, rbp);
        SAVE_GPR(rbx, rbx);
        SAVE_GPR(rdx, rdx);
        SAVE_GPR(rcx, rcx);
        SAVE_GPR(rax, rax);
#undef SAVE_GPR
        for (size_t i = 0; i < 16; ++i) {
            const auto offset = offsetof(GuestEntryRegisterFrame, ymm) + i * 32;
            c.vmovdqu(yword[rsp + offset], Xbyak::Ymm{static_cast<int>(i)});
        }
        c.lea(rax, ptr[rsp + GuestEntryStackAdjustment]);
        c.mov(qword[rsp + offsetof(GuestEntryRegisterFrame, original_rsp)], rax);
        c.pushfq();
        c.pop(rax);
        c.mov(qword[rsp + offsetof(GuestEntryRegisterFrame, rflags)], rax);

        c.mov(rdi, rsp);
        c.mov(rsi, reinterpret_cast<uintptr_t>(user_data));
        c.mov(rax, reinterpret_cast<uintptr_t>(observer));
        c.call(rax);

        for (size_t i = 0; i < 16; ++i) {
            const auto offset = offsetof(GuestEntryRegisterFrame, ymm) + i * 32;
            c.vmovdqu(Xbyak::Ymm{static_cast<int>(i)}, yword[rsp + offset]);
        }
        c.push(qword[rsp + offsetof(GuestEntryRegisterFrame, rflags)]);
        c.popfq();
#define RESTORE_GPR(member, reg) c.mov(reg, qword[rsp + offsetof(GuestEntryRegisterFrame, member)])
        RESTORE_GPR(r15, r15);
        RESTORE_GPR(r14, r14);
        RESTORE_GPR(r13, r13);
        RESTORE_GPR(r12, r12);
        RESTORE_GPR(r11, r11);
        RESTORE_GPR(r10, r10);
        RESTORE_GPR(r9, r9);
        RESTORE_GPR(r8, r8);
        RESTORE_GPR(rdi, rdi);
        RESTORE_GPR(rsi, rsi);
        RESTORE_GPR(rbp, rbp);
        RESTORE_GPR(rbx, rbx);
        RESTORE_GPR(rdx, rdx);
        RESTORE_GPR(rcx, rcx);
        RESTORE_GPR(rax, rax);
#undef RESTORE_GPR
        c.lea(rsp, ptr[rsp + GuestEntryStackAdjustment]);

        // Internal relative branches must target the relocated instruction body, which follows
        // the register-preserving observer stub at the start of the trampoline.
        const VAddr relocated_start = reinterpret_cast<VAddr>(generator.getCurr());
        std::array<u8, 15> relocated{};
        size_t copied = 0;
        for (size_t i = 0; i < instruction_count; ++i) {
            const size_t length = instruction_lengths[i];
            const VAddr source_address = target + copied;
            const VAddr destination_address = reinterpret_cast<VAddr>(generator.getCurr());
            if (!RelocateEntryInstruction(original.data() + copied, source_address,
                                          relocated.data(), destination_address, length, target,
                                          relocated_start, stolen_length)) {
                generator.setSize(generated_begin - generator.getCode());
                return false;
            }
            generator.db(relocated.data(), length);
            copied += length;
        }
        c.jmp(reinterpret_cast<const void*>(target + stolen_length),
              Xbyak::CodeGenerator::LabelType::T_NEAR);
        if (reinterpret_cast<VAddr>(generator.getCurr()) >
            reinterpret_cast<VAddr>(module.trampoline_end)) {
            generator.setSize(generated_begin - generator.getCode());
            return false;
        }
        patch = *plan;
        return true;
    } catch (const Xbyak::Error&) {
        generator.setSize(generated_begin - generator.getCode());
        return false;
    }
}

[[nodiscard]] static bool QueryPatchMapping(MemoryManager& memory, VAddr target,
                                            size_t patch_size, u32& protection,
                                            VAddr* mapping_end = nullptr) {
    if (patch_size == 0 || target > (std::numeric_limits<VAddr>::max)() - patch_size) return false;
    void* range_start{};
    void* range_end{};
    if (memory.QueryProtection(target, &range_start, &range_end, &protection) != 0) return false;
    const auto begin = reinterpret_cast<VAddr>(range_start);
    const auto end = reinterpret_cast<VAddr>(range_end);
    const u32 required = static_cast<u32>(MemoryProt::CpuRead) |
                         static_cast<u32>(MemoryProt::CpuExec);
    if (begin > target || end < target + patch_size || (protection & required) != required) {
        return false;
    }
    if (mapping_end != nullptr) *mapping_end = end;
    return true;
}

[[nodiscard]] static MemoryProt WritableExecutableProtection(u32 protection) {
    return static_cast<MemoryProt>(protection | static_cast<u32>(MemoryProt::CpuWrite));
}

} // namespace

bool InstallGuestEntryObserver(MemoryManager& memory, VAddr target, GuestEntryObserver observer,
                                void* user_data, GuestEntryObserverHandle& handle) {
#if !defined(ARCH_X86_64) || defined(SHADPS4_ENABLE_FEX_GUEST_CPU)
    (void)memory;
    (void)target;
    (void)observer;
    (void)user_data;
    (void)handle;
    return false;
#else
    if (handle.active || target == 0 || observer == nullptr) return false;
    auto* module = GetModule(reinterpret_cast<const void*>(target));
    if (module == nullptr) return false;
    std::unique_lock lock{module->mutex};
    auto* target_bytes = reinterpret_cast<u8*>(target);
    const VAddr module_start = reinterpret_cast<VAddr>(module->start);
    const VAddr module_end = reinterpret_cast<VAddr>(module->end);
    if (target < module_start || target >= module_end) return false;

    u32 previous_protection{};
    VAddr mapping_end{};
    if (!QueryPatchMapping(memory, target, 5, previous_protection, &mapping_end)) return false;
    EntryPatchPlan patch;
    VAddr trampoline_entry{};
    if (!GenerateGuestEntryTrampoline(*module, target, mapping_end, observer, user_data, patch,
                                      trampoline_entry)) {
        return false;
    }
    if (!FlushCodeRange(reinterpret_cast<void*>(trampoline_entry),
                        static_cast<size_t>(module->trampoline_gen.getCurr() -
                                            reinterpret_cast<u8*>(trampoline_entry)))) {
        module->trampoline_gen.setSize(reinterpret_cast<u8*>(trampoline_entry) -
                                       module->trampoline_gen.getCode());
        return false;
    }
    u32 current_protection{};
    if (!QueryPatchMapping(memory, target, patch.size, current_protection) ||
        current_protection != previous_protection ||
        !MatchesOriginalEntryPatch(std::span<const u8>{target_bytes, patch.size}, patch)) {
        module->trampoline_gen.setSize(reinterpret_cast<u8*>(trampoline_entry) -
                                       module->trampoline_gen.getCode());
        return false;
    }

    const auto writable_protection = WritableExecutableProtection(previous_protection);
    if (memory.Protect(target, patch.size, writable_protection) != 0) {
        module->trampoline_gen.setSize(reinterpret_cast<u8*>(trampoline_entry) -
                                       module->trampoline_gen.getCode());
        return false;
    }
    if (!MatchesOriginalEntryPatch(std::span<const u8>{target_bytes, patch.size}, patch)) {
        (void)memory.Protect(target, patch.size, static_cast<MemoryProt>(previous_protection));
        module->trampoline_gen.setSize(reinterpret_cast<u8*>(trampoline_entry) -
                                       module->trampoline_gen.getCode());
        return false;
    }
    auto target_span = std::span<u8>{target_bytes, patch.size};
    const auto rewind_trampoline = [&] {
        module->trampoline_gen.setSize(reinterpret_cast<u8*>(trampoline_entry) -
                                       module->trampoline_gen.getCode());
    };
    const auto rollback_installed_patch = [&] {
        u32 current{};
        if (!QueryPatchMapping(memory, target, patch.size, current) ||
            memory.Protect(target, patch.size, WritableExecutableProtection(current)) != 0) {
            (void)memory.Protect(target, patch.size, static_cast<MemoryProt>(previous_protection));
            return false;
        }
        const bool restored = RestoreEntryPatchIfMatches(target_span, patch);
        const bool restored_flush = restored && FlushCodeRange(target_bytes, patch.size);
        const bool protection_restored =
            memory.Protect(target, patch.size, static_cast<MemoryProt>(previous_protection)) == 0;
        return restored && restored_flush && protection_restored;
    };
    if (!ApplyEntryPatchIfMatchesOriginal(target_span, patch)) {
        (void)memory.Protect(target, patch.size, static_cast<MemoryProt>(previous_protection));
        rewind_trampoline();
        return false;
    }
    if (!FlushCodeRange(target_bytes, patch.size)) {
        if (rollback_installed_patch()) rewind_trampoline();
        return false;
    }
    if (memory.Protect(target, patch.size, static_cast<MemoryProt>(previous_protection)) != 0) {
        if (rollback_installed_patch()) rewind_trampoline();
        return false;
    }

    handle.target = target;
    handle.patch = patch;
    handle.original_protection = previous_protection;
    handle.active = true;
    return true;
#endif
}

bool RemoveGuestEntryObserver(MemoryManager& memory, GuestEntryObserverHandle& handle,
                              GuestThreadQuiescence quiescence) {
#if !defined(ARCH_X86_64) || defined(SHADPS4_ENABLE_FEX_GUEST_CPU)
    (void)memory;
    (void)handle;
    (void)quiescence;
    return false;
#else
    if (quiescence != GuestThreadQuiescence::Confirmed || !handle.active || handle.target == 0 ||
        handle.patch.size < 5 ||
        handle.patch.size > EntryPatchPlan::MaxBytes) {
        return false;
    }
    auto* module = GetModule(reinterpret_cast<const void*>(handle.target));
    if (module == nullptr) return false;
    std::unique_lock lock{module->mutex};
    auto* target_bytes = reinterpret_cast<u8*>(handle.target);
    const VAddr module_start = reinterpret_cast<VAddr>(module->start);
    const VAddr module_end = reinterpret_cast<VAddr>(module->end);
    if (handle.target < module_start || handle.target >= module_end) return false;

    u32 previous_protection{};
    if (!QueryPatchMapping(memory, handle.target, handle.patch.size, previous_protection) ||
        previous_protection != handle.original_protection ||
        !MatchesInstalledEntryPatch(std::span<const u8>{target_bytes, handle.patch.size},
                                    handle.patch)) {
        return false;
    }
    if (memory.Protect(handle.target, handle.patch.size,
                       WritableExecutableProtection(previous_protection)) != 0) {
        return false;
    }
    auto target_span = std::span<u8>{target_bytes, handle.patch.size};
    const auto reinstate_patch = [&] {
        u32 current{};
        if (!QueryPatchMapping(memory, handle.target, handle.patch.size, current) ||
            memory.Protect(handle.target, handle.patch.size, WritableExecutableProtection(current)) !=
                0) {
            (void)memory.Protect(handle.target, handle.patch.size,
                                 static_cast<MemoryProt>(previous_protection));
            handle.active = MatchesInstalledEntryPatch(target_span, handle.patch);
            return false;
        }
        const bool applied = ApplyEntryPatchIfMatchesOriginal(target_span, handle.patch);
        const bool flushed = applied && FlushCodeRange(target_bytes, handle.patch.size);
        const bool protection_restored =
            memory.Protect(handle.target, handle.patch.size,
                           static_cast<MemoryProt>(previous_protection)) == 0;
        handle.active = MatchesInstalledEntryPatch(target_span, handle.patch);
        return applied && flushed && protection_restored;
    };
    if (!MatchesInstalledEntryPatch(target_span, handle.patch)) {
        (void)memory.Protect(handle.target, handle.patch.size,
                             static_cast<MemoryProt>(previous_protection));
        return false;
    }
    if (!RestoreEntryPatchIfMatches(target_span, handle.patch)) {
        (void)memory.Protect(handle.target, handle.patch.size,
                             static_cast<MemoryProt>(previous_protection));
        return false;
    }
    if (!FlushCodeRange(target_bytes, handle.patch.size)) {
        (void)reinstate_patch();
        return false;
    }
    if (memory.Protect(handle.target, handle.patch.size,
                       static_cast<MemoryProt>(previous_protection)) != 0) {
        (void)reinstate_patch();
        return false;
    }
    handle.active = false;
    return true;
#endif
}

void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size) {
    std::call_once(init_flag, PatchesInit);

    const auto module_addr = reinterpret_cast<u64>(module_ptr);
    modules.emplace(std::piecewise_construct, std::forward_as_tuple(module_addr),
                    std::forward_as_tuple(static_cast<u8*>(module_ptr), module_size,
                                          static_cast<u8*>(trampoline_area_ptr),
                                          trampoline_area_size));
}

void PrePatchInstructions(u64 segment_addr, u64 segment_size) {
#if !defined(_WIN32) && !defined(__APPLE__)
    // Linux and others have an FS segment pointing to valid memory, so continue to do full
    // ahead-of-time patching for now until a better solution is worked out.
    if (!Patches.empty()) {
        TryPatchAot(reinterpret_cast<void*>(segment_addr), segment_size);
    }
#endif
}

} // namespace Core
