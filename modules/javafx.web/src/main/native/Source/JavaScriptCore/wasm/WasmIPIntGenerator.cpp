/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmIPIntGenerator.h"

#if ENABLE(WEBASSEMBLY)

#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeStructs.h"
#include "InstructionStream.h"
#include "JSCJSValueInlines.h"
#include "Label.h"
#include "WasmCallingConvention.h"
#include "WasmContext.h"
#include "WasmFunctionIPIntMetadataGenerator.h"
#include "WasmFunctionParser.h"
#include "WasmGeneratorTraits.h"
#include <variant>
#include <wtf/CompletionHandler.h>
#include <wtf/RefPtr.h>

/*
 * WebAssembly in-place interpreter metadata generator
 *
 * docs by Daniel Liu <daniel_liu4@apple.com / danlliu@umich.edu>; 2023 intern project
 *
 * 1. Why Metadata?
 * ----------------
 *
 * WebAssembly's bytecode format isn't always the easiest to interpret by itself: jumps would require parsing
 * through many bytes to find their target, constants are stored in LEB128, and a myriad of other reasons.
 * For IPInt, we design metadata to act as "supporting information" for the interpreter, allowing it to quickly
 * find important values such as constants, indices, and branch targets.
 *
 * 2. Metadata Structure
 * ---------------------
 *
 * Metadata is kept in a vector of UInt8 (bytes). We handle metadata in "metadata entries", which are groups of
 * 8 metadata bytes. We keep metadata aligned to 8B to improve access times. Sometimes, this results in higher
 * memory overhead; however, these cases are relatively sparse. Each instruction pushes a certain number of
 * entries to the metadata vector.
 *
 * 3. Metadata for Instructions
 * ----------------------------
 *
 * block (0x02):            1 entry; 8B PC of next instruction
 * loop (0x03):             1 entry; 8B PC of next instruction
 * if (0x04):               2 entries; 4B new PC, 4B new MC for `else`, 8B new PC for `if`
 * else (0x05):             1 entry; 4B new PC, 4B new MC for `end`
 * end (0x0b):              If exiting the function: ceil((# return values + 2) / 8) entries; 2B for total entry size, 1B / value returned
 * br (0x0c):               2 entries; 4B new PC, 4B new MC, 2B number of values to pop, 2B arity, 4B PC after br
 * br_if (0x0d):            2 entries; same as br
 * br_table (0x0e):         1 + 2n entries for n branches: 8B number of targets; n br metadata entries
 * local.get (0x20):        1 entry; 4B index of local, 4B size of instruction
 * local.set (0x21):        1 entry; 4B index of local, 4B size of instruction
 * local.tee (0x22):        2 entries because of how FunctionParser works
 * global.get (0x23):       1 entry; 4B index of global, 4B size of instruction
 * global.set (0x24):       1 entry; 4B index of global, 4B size of instruction
 * table.get (0x23):        1 entry; 4B index of table, 4B size of instruction
 * table.set (0x24):        1 entry; 4B index of table, 4B size of instruction
 * mem load (0x28 - 0x35):  1 entry; 4B memarg, 4B size of instruction
 * mem store (0x28 - 0x35): 1 entry; 4B memarg, 4B size of instruction
 * i32.const (0x41):        1 entry; 4B value, 4B size of instruction
 * i64.const (0x42):        2 entries; 8B value, 8B size of instruction
 *
 * i32, i64, f32, and f64 operations (besides the ones shown above) do not require metadata
 *
 */

namespace JSC { namespace Wasm {

using ErrorType = String;
using PartialResult = Expected<void, ErrorType>;
using UnexpectedResult = Unexpected<ErrorType>;
struct Value { };

// ControlBlock

struct IPIntControlType {

    friend class IPIntGenerator;

    IPIntControlType()
    {
    }

    IPIntControlType(BlockSignature signature, BlockType blockType, CatchKind catchKind = CatchKind::Catch)
        : m_signature(signature)
        , m_blockType(blockType)
        , m_catchKind(catchKind)
    { }

    static bool isIf(const IPIntControlType& control) { return control.blockType() == BlockType::If; }
    static bool isTry(const IPIntControlType& control) { return control.blockType() == BlockType::Try; }
    static bool isAnyCatch(const IPIntControlType& control) { return control.blockType() == BlockType::Catch; }
    static bool isTopLevel(const IPIntControlType& control) { return control.blockType() == BlockType::TopLevel; }
    static bool isLoop(const IPIntControlType& control) { return control.blockType() == BlockType::Loop; }
    static bool isBlock(const IPIntControlType& control) { return control.blockType() == BlockType::Block; }
    static bool isCatch(const IPIntControlType& control)
    {
        if (control.blockType() != BlockType::Catch)
            return false;
        return control.catchKind() == CatchKind::Catch;
    }

    void dump(PrintStream&) const
    { }

    BlockType blockType() const { return m_blockType; }
    CatchKind catchKind() const { return m_catchKind; }
    BlockSignature signature() const { return m_signature; }

    Type branchTargetType(unsigned i) const
    {
        ASSERT(i < branchTargetArity());
        if (blockType() == BlockType::Loop)
            return m_signature->as<FunctionSignature>()->argumentType(i);
        return m_signature->as<FunctionSignature>()->returnType(i);
    }

    unsigned branchTargetArity() const
    {
        return isLoop(*this)
            ? m_signature->as<FunctionSignature>()->argumentCount()
            : m_signature->as<FunctionSignature>()->returnCount();
    }

private:
    BlockSignature m_signature;
    BlockType m_blockType;
    CatchKind m_catchKind;

    Vector<uint32_t> m_awaitingUpdate;
    int32_t m_pendingOffset = -1;
    int32_t m_pc = -1;
    int32_t m_mc = -1;
};

class IPIntGenerator {
public:
    IPIntGenerator(ModuleInformation&, unsigned, const TypeDefinition&, const uint8_t*, const uint32_t);

    using ControlType = IPIntControlType;
    using ExpressionType = Value;
    using CallType = CallLinkInfo::CallType;
    using ResultList = Vector<Value, 8>;

    using ExpressionList = Vector<Value, 1>;
    using ControlEntry = FunctionParser<IPIntGenerator>::ControlEntry;
    using ControlStack = FunctionParser<IPIntGenerator>::ControlStack;
    using Stack = FunctionParser<IPIntGenerator>::Stack;
    using TypedExpression = FunctionParser<IPIntGenerator>::TypedExpression;

    static ExpressionType emptyExpression() { return { }; };
    PartialResult WARN_UNUSED_RETURN addDrop(ExpressionType);

    template <typename ...Args>
    NEVER_INLINE UnexpectedResult WARN_UNUSED_RETURN fail(Args... args) const
    {
        using namespace FailureHelper; // See ADL comment in WasmParser.h.
        return UnexpectedResult(makeString("WebAssembly.Module failed compiling: "_s, makeString(args)...));
    }
#define WASM_COMPILE_FAIL_IF(condition, ...) do { \
        if (UNLIKELY(condition))                  \
            return fail(__VA_ARGS__);             \
    } while (0)

    std::unique_ptr<FunctionIPIntMetadataGenerator> finalize();

    PartialResult WARN_UNUSED_RETURN addArguments(const TypeDefinition&);
    PartialResult WARN_UNUSED_RETURN addLocal(Type, uint32_t);
    Value addConstant(Type, uint64_t);

    // SIMD

    void notifyFunctionUsesSIMD() { }
    PartialResult WARN_UNUSED_RETURN addSIMDLoad(ExpressionType, uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDStore(ExpressionType, ExpressionType, uint32_t);
    PartialResult WARN_UNUSED_RETURN addSIMDSplat(SIMDLane, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDShuffle(v128_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDShift(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDExtmul(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadSplat(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDStoreLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadExtend(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadPad(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&);

    Value WARN_UNUSED_RETURN addConstant(v128_t);

    // SIMD generated

    PartialResult WARN_UNUSED_RETURN addExtractLane(SIMDInfo, uint8_t, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addReplaceLane(SIMDInfo, uint8_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDI_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDV_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDBitwiseSelect(ExpressionType, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDRelOp(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, B3::Air::Arg, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSIMDV_VV(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&);

    // References

    PartialResult WARN_UNUSED_RETURN addRefIsNull(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addRefFunc(uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addRefAsNonNull(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addRefEq(ExpressionType, ExpressionType, ExpressionType&);

    // Tables

    PartialResult WARN_UNUSED_RETURN addTableGet(unsigned, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addTableSet(unsigned, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addTableInit(unsigned, unsigned, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addElemDrop(unsigned);
    PartialResult WARN_UNUSED_RETURN addTableSize(unsigned, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addTableGrow(unsigned, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addTableFill(unsigned, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addTableCopy(unsigned, unsigned, ExpressionType, ExpressionType, ExpressionType);

    // Locals

    PartialResult WARN_UNUSED_RETURN getLocal(uint32_t index, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN setLocal(uint32_t, ExpressionType);

    // Globals

    PartialResult WARN_UNUSED_RETURN getGlobal(uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN setGlobal(uint32_t, ExpressionType);

    // Memory

    PartialResult WARN_UNUSED_RETURN load(LoadOpType, ExpressionType, ExpressionType&, uint32_t);
    PartialResult WARN_UNUSED_RETURN store(StoreOpType, ExpressionType, ExpressionType, uint32_t);
    PartialResult WARN_UNUSED_RETURN addGrowMemory(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addCurrentMemory(ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addMemoryFill(ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addMemoryCopy(ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addMemoryInit(unsigned, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addDataDrop(unsigned);

    // Atomics

    PartialResult WARN_UNUSED_RETURN atomicLoad(ExtAtomicOpType, Type, ExpressionType, ExpressionType&, uint32_t);
    PartialResult WARN_UNUSED_RETURN atomicStore(ExtAtomicOpType, Type, ExpressionType, ExpressionType, uint32_t);
    PartialResult WARN_UNUSED_RETURN atomicBinaryRMW(ExtAtomicOpType, Type, ExpressionType, ExpressionType, ExpressionType&, uint32_t);
    PartialResult WARN_UNUSED_RETURN atomicCompareExchange(ExtAtomicOpType, Type, ExpressionType, ExpressionType, ExpressionType, ExpressionType&, uint32_t);

    PartialResult WARN_UNUSED_RETURN atomicWait(ExtAtomicOpType, ExpressionType, ExpressionType, ExpressionType, ExpressionType&, uint32_t);
    PartialResult WARN_UNUSED_RETURN atomicNotify(ExtAtomicOpType, ExpressionType, ExpressionType, ExpressionType&, uint32_t);
    PartialResult WARN_UNUSED_RETURN atomicFence(ExtAtomicOpType, uint8_t);

    // Saturated truncation

    PartialResult WARN_UNUSED_RETURN truncSaturated(Ext1OpType, ExpressionType, ExpressionType&, Type, Type);

    // GC

    PartialResult WARN_UNUSED_RETURN addI31New(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI31GetS(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI31GetU(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayNew(uint32_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayNewDefault(uint32_t, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayNewData(uint32_t, uint32_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayNewElem(uint32_t, uint32_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayNewFixed(uint32_t, Vector<ExpressionType>&, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArrayGet(ExtGCOpType, uint32_t, ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addArraySet(uint32_t, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addArrayLen(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addStructNew(uint32_t, Vector<ExpressionType>&, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addStructNewDefault(uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addStructGet(ExpressionType, const StructType&, uint32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addStructSet(ExpressionType, const StructType&, uint32_t, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addRefTest(ExpressionType, bool, int32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addRefCast(ExpressionType, bool, int32_t, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addExternInternalize(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addExternExternalize(ExpressionType, ExpressionType&);

    // Basic operators

    PartialResult WARN_UNUSED_RETURN addI32DivS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32RemS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32DivU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32RemU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64DivS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64RemS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64DivU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64RemU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Ctz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Popcnt(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Popcnt(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Nearest(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Nearest(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Trunc(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Trunc(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32TruncSF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32TruncSF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32TruncUF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32TruncUF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64TruncSF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64TruncSF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64TruncUF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64TruncUF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Ceil(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Mul(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Sub(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Le(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32DemoteF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Ne(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Lt(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Min(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Max(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Min(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Max(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Mul(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Div(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Clz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Copysign(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32ReinterpretI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Ne(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Gt(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Sqrt(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Ge(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64GtS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64GtU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Div(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Add(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32LeU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32LeS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Ne(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Clz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Neg(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32And(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32LtU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Rotr(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Abs(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32LtS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Eq(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Copysign(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32ConvertSI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Rotl(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Lt(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64ConvertSI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Eq(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Le(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Ge(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32ShrU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32ConvertUI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32ShrS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32GeU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Ceil(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32GeS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Shl(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Floor(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Xor(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Abs(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Mul(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Sub(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32ReinterpretF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Add(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Sub(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Or(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64LtU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64LtS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64ConvertSI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Xor(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64GeU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Mul(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Sub(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64PromoteF32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Add(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64GeS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64ExtendUI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Ne(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64ReinterpretI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Eq(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Eq(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Floor(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32ConvertSI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64And(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Or(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Ctz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Eqz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Eqz(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64ReinterpretF64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64ConvertUI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32ConvertUI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64ConvertUI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64ShrS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64ShrU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Sqrt(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Shl(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF32Gt(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32WrapI64(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Rotl(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Rotr(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32GtU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64ExtendSI32(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Extend8S(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32Extend16S(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Extend8S(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Extend16S(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Extend32S(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI32GtS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addF64Neg(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64LeU(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64LeS(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addI64Add(ExpressionType, ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addSelect(ExpressionType, ExpressionType, ExpressionType, ExpressionType&);

    // Control flow

    ControlType WARN_UNUSED_RETURN addTopLevel(BlockSignature);
    PartialResult WARN_UNUSED_RETURN addBlock(BlockSignature, Stack&, ControlType&, Stack&);
    PartialResult WARN_UNUSED_RETURN addLoop(BlockSignature, Stack&, ControlType&, Stack&, uint32_t);
    PartialResult WARN_UNUSED_RETURN addIf(ExpressionType, BlockSignature, Stack&, ControlType&, Stack&);
    PartialResult WARN_UNUSED_RETURN addElse(ControlType&, Stack&);
    PartialResult WARN_UNUSED_RETURN addElseToUnreachable(ControlType&);

    PartialResult WARN_UNUSED_RETURN addTry(BlockSignature, Stack&, ControlType&, Stack&);
    PartialResult WARN_UNUSED_RETURN addCatch(unsigned, const TypeDefinition&, Stack&, ControlType&, ResultList&);
    PartialResult WARN_UNUSED_RETURN addCatchToUnreachable(unsigned, const TypeDefinition&, ControlType&, ResultList&);
    PartialResult WARN_UNUSED_RETURN addCatchAll(Stack&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addCatchAllToUnreachable(ControlType&);
    PartialResult WARN_UNUSED_RETURN addDelegate(ControlType&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addDelegateToUnreachable(ControlType&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addThrow(unsigned, Vector<ExpressionType>&, Stack&);
    PartialResult WARN_UNUSED_RETURN addRethrow(unsigned, ControlType&);

    PartialResult WARN_UNUSED_RETURN addReturn(const ControlType&, const Stack&);
    PartialResult WARN_UNUSED_RETURN addBranch(ControlType&, ExpressionType, const Stack&);
    PartialResult WARN_UNUSED_RETURN addSwitch(ExpressionType, const Vector<ControlType*>&, ControlType&, const Stack&);
    PartialResult WARN_UNUSED_RETURN endBlock(ControlEntry&, Stack&);
    PartialResult WARN_UNUSED_RETURN addEndToUnreachable(ControlEntry&, Stack&);

    PartialResult WARN_UNUSED_RETURN endTopLevel(BlockSignature, const Stack&) { return { }; }

    // Calls

    PartialResult WARN_UNUSED_RETURN addCall(uint32_t, const TypeDefinition&, Vector<ExpressionType>&, ResultList&, CallType = CallType::Call);
    PartialResult WARN_UNUSED_RETURN addCallIndirect(unsigned, const TypeDefinition&, Vector<ExpressionType>&, ResultList&, CallType = CallType::Call);
    PartialResult WARN_UNUSED_RETURN addCallRef(const TypeDefinition&, Vector<ExpressionType>&, ResultList&);
    PartialResult WARN_UNUSED_RETURN addUnreachable();
    PartialResult WARN_UNUSED_RETURN addCrash();

    void setParser(FunctionParser<IPIntGenerator>* parser) { m_parser = parser; };
    size_t getCurrentInstructionLength()
    {
        return m_parser->offset() - m_parser->currentOpcodeStartingOffset();
    }
    void addCallCommonData(const FunctionSignature&);
    void didFinishParsingLocals()
    {
        m_metadata->m_bytecodeOffset = m_parser->offset();
    }
    void didPopValueFromStack(ExpressionType, String) { }
    void willParseOpcode() { }
    void didParseOpcode() { }
    void dump(const ControlStack&, const Stack*) { }

    static constexpr bool tierSupportsSIMD = false;
private:
    FunctionParser<IPIntGenerator>* m_parser { nullptr };
    ModuleInformation& m_info;
    std::unique_ptr<FunctionIPIntMetadataGenerator> m_metadata;
};

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addDrop(ExpressionType) { return { }; }

IPIntGenerator::IPIntGenerator(ModuleInformation& info, unsigned functionIndex, const TypeDefinition&, const uint8_t* bytecode, const uint32_t bytecode_len)
    : m_info(info)
    , m_metadata(WTF::makeUnique<FunctionIPIntMetadataGenerator>(functionIndex, bytecode, bytecode_len))
{
    UNUSED_PARAM(info);
}

Value IPIntGenerator::addConstant(Type type, uint64_t value)
{
    m_metadata->addLEB128ConstantAndLengthForType(type, value, getCurrentInstructionLength());
    return { };
}

// SIMD

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDLoad(ExpressionType, uint32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDStore(ExpressionType, ExpressionType, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDSplat(SIMDLane, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDShuffle(v128_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDShift(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDExtmul(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDLoadSplat(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDLoadLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDStoreLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDLoadExtend(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDLoadPad(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&) { return { }; }

Value WARN_UNUSED_RETURN IPIntGenerator::addConstant(v128_t)
{
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addExtractLane(SIMDInfo, uint8_t, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addReplaceLane(SIMDInfo, uint8_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDI_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDV_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDBitwiseSelect(ExpressionType, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDRelOp(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, B3::Air::Arg, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSIMDV_VV(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&) { return { }; }

// References

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefIsNull(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefFunc(uint32_t index, ExpressionType&)
{
    m_metadata->addLEB128ConstantInt32AndLength(index, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefAsNonNull(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefEq(ExpressionType, ExpressionType, ExpressionType&) { return { }; }

// Tables

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableGet(unsigned index, ExpressionType, ExpressionType&)
{
    m_metadata->addLEB128ConstantInt32AndLength(index, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableSet(unsigned index, ExpressionType, ExpressionType)
{
    m_metadata->addLEB128ConstantInt32AndLength(index, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableInit(unsigned elementIndex, unsigned tableIndex, ExpressionType, ExpressionType, ExpressionType)
{
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(16);
    auto tableInitData = m_metadata->m_metadata.data() + size;
    WRITE_TO_METADATA(tableInitData, elementIndex, uint32_t);
    WRITE_TO_METADATA(tableInitData + 4, tableIndex, uint32_t);
    WRITE_TO_METADATA(tableInitData + 8, getCurrentInstructionLength(), uint64_t);
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addElemDrop(unsigned elementIndex)
{
    m_metadata->addLEB128ConstantInt32AndLength(elementIndex, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableSize(unsigned tableIndex, ExpressionType&)
{
    m_metadata->addLEB128ConstantInt32AndLength(tableIndex, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableGrow(unsigned tableIndex, ExpressionType, ExpressionType, ExpressionType&)
{
    m_metadata->addLEB128ConstantInt32AndLength(tableIndex, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableFill(unsigned tableIndex, ExpressionType, ExpressionType, ExpressionType)
{
    m_metadata->addLEB128ConstantInt32AndLength(tableIndex, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTableCopy(unsigned dstTableIndex, unsigned srcTableIndex, ExpressionType, ExpressionType, ExpressionType)
{
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(16);
    auto tableInitData = m_metadata->m_metadata.data() + size;
    WRITE_TO_METADATA(tableInitData, dstTableIndex, uint32_t);
    WRITE_TO_METADATA(tableInitData + 4, srcTableIndex, uint32_t);
    WRITE_TO_METADATA(tableInitData + 8, getCurrentInstructionLength(), uint64_t);
    return { };
}

// Locals and Globals

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArguments(const TypeDefinition &signature)
{
    auto sig = signature.as<FunctionSignature>();
    auto numArgs = sig->argumentCount();
    m_metadata->m_numLocals += numArgs;
    m_metadata->m_numArguments = numArgs;
    m_metadata->m_argumentLocations.resize(numArgs);

    int numGPR = 0;
    int numFPR = 0;
    int stackOffset = 16;

    for (size_t i = 0; i < numArgs; ++i) {
        auto arg = sig->argumentType(i);
        if (arg.isI32() || arg.isI64()) {
            if (numGPR < 8)
                m_metadata->m_argumentLocations[i] = numGPR++;
            else {
                m_metadata->m_numArgumentsOnStack++;
                m_metadata->m_argumentLocations[i] = stackOffset++;
            }
        } else {
            if (numFPR < 8)
                m_metadata->m_argumentLocations[i] = 8 + numFPR++;
            else {
                m_metadata->m_numArgumentsOnStack++;
                m_metadata->m_argumentLocations[i] = stackOffset++;
            }
        }
    }
    m_metadata->m_nonArgLocalOffset = 16 + m_metadata->m_numArgumentsOnStack - m_metadata->m_numArguments;
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addLocal(Type, uint32_t count)
{
    m_metadata->m_numLocals += count;
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::getLocal(uint32_t index, ExpressionType&)
{
    if (index >= m_metadata->m_numArguments)
        m_metadata->addLEB128ConstantInt32AndLength(index + m_metadata->m_nonArgLocalOffset, getCurrentInstructionLength());
    else
        m_metadata->addLEB128ConstantInt32AndLength(m_metadata->m_argumentLocations[index], getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::setLocal(uint32_t index, ExpressionType)
{
    if (index >= m_metadata->m_numArguments)
        m_metadata->addLEB128ConstantInt32AndLength(index + m_metadata->m_nonArgLocalOffset, getCurrentInstructionLength());
    else
        m_metadata->addLEB128ConstantInt32AndLength(m_metadata->m_argumentLocations[index], getCurrentInstructionLength());
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::getGlobal(uint32_t index, ExpressionType&)
{
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(8);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size, index, uint32_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 4, getCurrentInstructionLength(), uint16_t);
    const Wasm::GlobalInformation& global = m_info.globals[index];
    switch (global.bindingMode) {
    case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance:
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 6, 0, uint16_t);
        break;
    case Wasm::GlobalInformation::BindingMode::Portable:
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 6, 1, uint16_t);
        break;
    }
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::setGlobal(uint32_t index, ExpressionType)
{
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(8);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size, index, uint32_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 4, getCurrentInstructionLength(), uint16_t);
    const Wasm::GlobalInformation& global = m_info.globals[index];
    switch (global.bindingMode) {
    case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance:
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 6, 0, uint8_t);
        break;
    case Wasm::GlobalInformation::BindingMode::Portable:
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 6, 1, uint8_t);
        break;
    }
    if (isRefType(m_info.globals[index].type))
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 7, 1, uint8_t);
    else
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 7, 0, uint8_t);
    return { };
}

// Loads and Stores

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::load(LoadOpType, ExpressionType, ExpressionType&, uint32_t offset)
{
    m_metadata->addLEB128ConstantInt32AndLength(offset, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::store(StoreOpType, ExpressionType, ExpressionType, uint32_t offset)
{
    m_metadata->addLEB128ConstantInt32AndLength(offset, getCurrentInstructionLength());
    return { };
}

// Memories

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addGrowMemory(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCurrentMemory(ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addMemoryFill(ExpressionType, ExpressionType, ExpressionType) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addMemoryCopy(ExpressionType, ExpressionType, ExpressionType) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addMemoryInit(unsigned dataIndex, ExpressionType, ExpressionType, ExpressionType)
{
    m_metadata->addLEB128ConstantInt32AndLength(dataIndex, getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addDataDrop(unsigned dataIndex)
{
    m_metadata->addLEB128ConstantInt32AndLength(dataIndex, getCurrentInstructionLength());
    return { };
}

// Atomics

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicLoad(ExtAtomicOpType, Type, ExpressionType, ExpressionType&, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicStore(ExtAtomicOpType, Type, ExpressionType, ExpressionType, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicBinaryRMW(ExtAtomicOpType, Type, ExpressionType, ExpressionType, ExpressionType&, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicCompareExchange(ExtAtomicOpType, Type, ExpressionType, ExpressionType, ExpressionType, ExpressionType&, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicWait(ExtAtomicOpType, ExpressionType, ExpressionType, ExpressionType, ExpressionType&, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicNotify(ExtAtomicOpType, ExpressionType, ExpressionType, ExpressionType&, uint32_t) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::atomicFence(ExtAtomicOpType, uint8_t) { return { }; }

// GC

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI31New(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI31GetS(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI31GetU(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayNew(uint32_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayNewData(uint32_t, uint32_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayNewElem(uint32_t, uint32_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayNewFixed(uint32_t, Vector<ExpressionType>&, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayNewDefault(uint32_t, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayGet(ExtGCOpType, uint32_t, ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArraySet(uint32_t, ExpressionType, ExpressionType, ExpressionType) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addArrayLen(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addStructNew(uint32_t, Vector<ExpressionType>&, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addStructNewDefault(uint32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addStructGet(ExpressionType, const StructType&, uint32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addStructSet(ExpressionType, const StructType&, uint32_t, ExpressionType) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefTest(ExpressionType, bool, int32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRefCast(ExpressionType, bool, int32_t, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addExternInternalize(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addExternExternalize(ExpressionType, ExpressionType&) { return { }; }

// Integer Arithmetic

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Add(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Add(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Sub(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Sub(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Mul(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Mul(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32DivS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32DivU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64DivS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64DivU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32RemS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32RemU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64RemS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64RemU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }

// Bitwise Operations

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32And(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64And(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Xor(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Xor(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Or(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Or(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Shl(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32ShrU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32ShrS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Shl(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64ShrU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64ShrS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Rotl(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Rotl(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Rotr(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Rotr(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Popcnt(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Popcnt(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Clz(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Clz(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Ctz(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Ctz(ExpressionType, ExpressionType&) { return { }; }

// Floating-Point Arithmetic

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Add(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Add(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Sub(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Sub(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Mul(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Mul(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Div(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Div(ExpressionType, ExpressionType, ExpressionType&) { return { }; }

// Other Floating-Point Instructions

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Min(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Max(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Min(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Max(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Nearest(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Nearest(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Floor(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Floor(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Ceil(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Ceil(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Copysign(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Copysign(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Sqrt(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Sqrt(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Neg(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Neg(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Abs(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Abs(ExpressionType, ExpressionType&) { return { }; }

// Integer Comparisons

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Eq(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Ne(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32LtS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32LtU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32LeS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32LeU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32GtS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32GtU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32GeU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32GeS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Eqz(ExpressionType, ExpressionType&) { return { }; }

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Eq(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Ne(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64GtS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64GtU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64GeS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64GeU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64LtS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64LtU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64LeS(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64LeU(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Eqz(ExpressionType, ExpressionType&) { return { }; }

// Floating-Point Comparisons

// Implementation status: No need for metadata, DONE.

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Eq(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Ne(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Lt(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Le(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Gt(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Ge(ExpressionType, ExpressionType, ExpressionType&) { return { }; }

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Eq(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Ne(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Lt(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Le(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Gt(ExpressionType, ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Ge(ExpressionType, ExpressionType, ExpressionType&) { return { }; }

// Integer Extension

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64ExtendSI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64ExtendUI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Extend8S(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32Extend16S(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Extend8S(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Extend16S(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64Extend32S(ExpressionType, ExpressionType&) { return { }; }

// Truncation

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64Trunc(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32Trunc(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32TruncSF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32TruncSF32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32TruncUF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32TruncUF32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64TruncSF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64TruncSF32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64TruncUF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64TruncUF32(ExpressionType, ExpressionType&) { return { }; }

PartialResult WARN_UNUSED_RETURN IPIntGenerator::truncSaturated(Ext1OpType, ExpressionType, ExpressionType&, Type, Type) { return { }; }

// Conversions

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32WrapI64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32DemoteF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64PromoteF32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32ReinterpretI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI32ReinterpretF32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64ReinterpretI64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addI64ReinterpretF64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32ConvertSI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32ConvertUI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32ConvertSI64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF32ConvertUI64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64ConvertSI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64ConvertUI32(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64ConvertSI64(ExpressionType, ExpressionType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addF64ConvertUI64(ExpressionType, ExpressionType&) { return { }; }

// Control Flow Blocks

// Implementation status: UNIMPLEMENTED

IPIntGenerator::ControlType WARN_UNUSED_RETURN IPIntGenerator::addTopLevel(BlockSignature signature)
{
    return ControlType(signature, BlockType::TopLevel);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSelect(ExpressionType, ExpressionType, ExpressionType, ExpressionType&)
{
    m_metadata->addRawValue(getCurrentInstructionLength());
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addBlock(BlockSignature signature, Stack& oldStack, ControlType& block, Stack& newStack)
{
    splitStack(signature, oldStack, newStack);
    block = ControlType(signature, BlockType::Block);
    // next PC (to skip type signature)
    // m_metadata->addLEB128ConstantInt32AndLength(0, getCurrentInstructionLength());
    m_metadata->addRawValue(m_parser->offset() - m_metadata->m_bytecodeOffset);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addLoop(BlockSignature signature, Stack& oldStack, ControlType& block, Stack& newStack, uint32_t)
{
    splitStack(signature, oldStack, newStack);
    block = ControlType(signature, BlockType::Loop);
    block.m_pendingOffset = -1; // no need to update!
    // next PC (to skip type signature)
    m_metadata->addRawValue(m_parser->offset() - m_metadata->m_bytecodeOffset);
    // No -1 because we can just have it directly go to the instruction after
    // No point running `loop` since in IPInt it's just a nop
    block.m_pc = m_parser->offset() - m_metadata->m_bytecodeOffset;
    block.m_mc = m_metadata->m_metadata.size();
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addIf(ExpressionType, BlockSignature signature, Stack& oldStack, ControlType& block, Stack& newStack)
{
    splitStack(signature, oldStack, newStack);
    block = ControlType(signature, BlockType::If);
    block.m_pendingOffset = m_metadata->m_metadata.size();
    // 4B PC of else
    // 4B MC of else
    m_metadata->addBlankSpace(8);
    // 8B PC of if (skip type signature)
    m_metadata->addRawValue(m_parser->offset() - m_metadata->m_bytecodeOffset);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addElse(ControlType& block, Stack& stack)
{
    const FunctionSignature& signature = *block.signature()->as<FunctionSignature>();
    stack.clear();
    for (unsigned i = 0; i < signature.argumentCount(); i ++)
        stack.constructAndAppend(signature.argumentType(i), Value { });
    return addElseToUnreachable(block);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addElseToUnreachable(ControlType& block)
{
    // New PC
    // size - 1 for index of last element
    // - bytecodeOffset since we index starting there in IPInt
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + block.m_pendingOffset, m_parser->offset() - m_metadata->m_bytecodeOffset, uint32_t);
    // New MC
    if (m_parser->currentOpcode() == OpType::End) {
        // Edge case: if ... end with no else: don't actually add in this metadata or else IPInt tries to read the else
        // New MC
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + block.m_pendingOffset + 4, m_metadata->m_metadata.size(), uint32_t);
        block = ControlType(block.signature(), BlockType::Block);
        block.m_pendingOffset = -1;
        return { };
    }
    // New MC
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + block.m_pendingOffset + 4, m_metadata->m_metadata.size() + 8, uint32_t);
    block = ControlType(block.signature(), BlockType::Block);
    block.m_pendingOffset = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(8);
    return { };
}

// Exception Handling

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addTry(BlockSignature signature, Stack& oldStack, ControlType& block, Stack& newStack)
{
    splitStack(signature, oldStack, newStack);
    block = ControlType(signature, BlockType::Try);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCatch(unsigned exceptionIndex, const TypeDefinition& exceptionSignature, Stack&, ControlType& block, ResultList& results)
{
    return addCatchToUnreachable(exceptionIndex, exceptionSignature, block, results);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCatchToUnreachable(unsigned, const TypeDefinition& exceptionSignature, ControlType& block, ResultList& results)
{
    const FunctionSignature& signature = *exceptionSignature.as<FunctionSignature>();
    for (unsigned i = 0; i < signature.argumentCount(); i ++)
        results.append(Value { });
    block = ControlType(block.signature(), BlockType::Catch);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCatchAll(Stack&, ControlType& block)
{
    return addCatchAllToUnreachable(block);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCatchAllToUnreachable(ControlType& block)
{
    block = ControlType(block.signature(), BlockType::Catch, CatchKind::CatchAll);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addDelegate(ControlType&, ControlType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addDelegateToUnreachable(ControlType&, ControlType&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addThrow(unsigned, Vector<ExpressionType>&, Stack&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addRethrow(unsigned, ControlType&) { return { }; }

// Control Flow Branches

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addReturn(const ControlType&, const Stack&) { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addBranch(ControlType& block, ExpressionType, const Stack& stack)
{
    auto size = m_metadata->m_metadata.size();
    block.m_awaitingUpdate.append(size);
    m_metadata->addBlankSpace(16);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 8, stack.size() - block.branchTargetArity(), uint16_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 10, block.branchTargetArity(), uint16_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size + 12, m_parser->offset() - m_metadata->m_bytecodeOffset, uint32_t);
    return { };
}
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addSwitch(ExpressionType, const Vector<ControlType*>& jumps, ControlType& defaultJump, const Stack& stack)
{
    auto size = m_metadata->m_metadata.size();
    // Metadata layout
    // 0 - 7     number of jump targets (including end)
    // 8 - 15    4B PC for t0, 4B MC for t0
    // 16 - 23   2B pop, 2B keep, 4B empty
    // 24 and on repeat for each branch target
    m_metadata->addBlankSpace(8);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + size, jumps.size() + 1, uint64_t);

    for (auto block : jumps) {
        auto jumpBase = m_metadata->m_metadata.size();
        m_metadata->addBlankSpace(16);
        block->m_awaitingUpdate.append(jumpBase);
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + jumpBase + 8, stack.size() - block->branchTargetArity(), uint16_t);
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + jumpBase + 10, block->branchTargetArity(), uint16_t);
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + jumpBase + 12, m_parser->offset() - m_metadata->m_bytecodeOffset, uint32_t);
    }
    auto defaultJumpBase = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(16);
    defaultJump.m_awaitingUpdate.append(defaultJumpBase);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + defaultJumpBase + 8, stack.size() - defaultJump.branchTargetArity(), uint16_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + defaultJumpBase + 10, defaultJump.branchTargetArity(), uint16_t);
    WRITE_TO_METADATA(m_metadata->m_metadata.data() + defaultJumpBase + 12, m_parser->offset() - m_metadata->m_bytecodeOffset, uint32_t);

    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::endBlock(ControlEntry& entry, Stack& stack)
{
    return addEndToUnreachable(entry, stack);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addEndToUnreachable(ControlEntry& entry, Stack&)
{
    auto block = entry.controlData;
    // if, else, block: set metadata of prior instruction to current location
    // if: jump forward for not taken
    // else: jump forward for if taken
    // block: jump forward for br inside
    if (ControlType::isIf(block)) {
        // New PC
        // size - 1 for index of last element
        // - bytecodeOffset since we index starting there in IPInt
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + entry.controlData.m_pendingOffset, m_parser->offset() - m_metadata->m_bytecodeOffset - 1, uint32_t);
        // New MC
        WRITE_TO_METADATA(m_metadata->m_metadata.data() + entry.controlData.m_pendingOffset + 4, m_metadata->m_metadata.size(), uint32_t);
    } else if (ControlType::isBlock(block)) {
        if (block.m_pendingOffset != -1) {
            // else
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + entry.controlData.m_pendingOffset, m_parser->offset() - m_metadata->m_bytecodeOffset - 1, uint32_t);
            // New MC
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + entry.controlData.m_pendingOffset + 4, m_metadata->m_metadata.size(), uint32_t);
        } else {
            // If it's a block, resolve all the jumps
            for (auto x : block.m_awaitingUpdate) {
                WRITE_TO_METADATA(m_metadata->m_metadata.data() + x, m_parser->offset() - m_metadata->m_bytecodeOffset - 1, uint32_t);
                // New MC
                WRITE_TO_METADATA(m_metadata->m_metadata.data() + x + 4, m_metadata->m_metadata.size(), uint32_t);
            }
        }
    } else if (ControlType::isLoop(block)) {
        for (auto x : block.m_awaitingUpdate) {
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + x, block.m_pc, uint32_t);
            // New MC
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + x + 4, block.m_mc, uint32_t);
        }
    } else if (ControlType::isTopLevel(block)) {
        // Final end
        for (auto x : block.m_awaitingUpdate) {
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + x, m_parser->offset() - m_metadata->m_bytecodeOffset - 1, uint32_t);
            // New MC
            WRITE_TO_METADATA(m_metadata->m_metadata.data() + x + 4, m_metadata->m_metadata.size(), uint32_t);
        }

        // Metadata = round up 8 bytes, one for each
        m_metadata->m_bytecodeLength = m_parser->offset();
        Vector<Type> types(entry.controlData.branchTargetArity());
        for (size_t i = 0; i < entry.controlData.branchTargetArity(); ++i)
            types[i] = entry.controlData.branchTargetType(i);
        m_metadata->addReturnData(types);
    }
    const FunctionSignature& signature = *entry.controlData.signature()->as<FunctionSignature>();
    for (unsigned i = 0; i < signature.returnCount(); i ++)
        entry.enclosedExpressionStack.constructAndAppend(signature.returnType(i), Value { });
    return { };
}

// Calls

// Implementation status: UNIMPLEMENTED

void IPIntGenerator::addCallCommonData(const FunctionSignature& signature)
{
    // Add function signature
    // 8B for offsets of GPR, 8B for offsets of FPR, 2B for length of total metadata, 2B for number of arguments, 2B per stack argument
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(32);
    auto signatureMetadata = m_metadata->m_metadata.data() + size;

    uint16_t stackOffset = signature.argumentCount() - 1;
    uint8_t gprsUsed = 0;
    uint8_t fprsUsed = 0;

    Vector<uint16_t> locations;

#if CPU(X86_64)
    uint8_t maxGPRs = 6;
#elif CPU(ARM64)
    uint8_t maxGPRs = 8;
#else
    uint8_t maxGPRs = 0;
#endif
    uint8_t maxFPRs = 8;

    WRITE_TO_METADATA(signatureMetadata, 0, uint64_t);
    WRITE_TO_METADATA(signatureMetadata + 8, 0, uint64_t);
    WRITE_TO_METADATA(signatureMetadata + 16, 0, uint64_t);
    WRITE_TO_METADATA(signatureMetadata + 24, 0, uint64_t);

    for (size_t i = 0; i < signature.argumentCount(); ++i, --stackOffset) {
        auto argType = signature.argumentType(i);
        if ((argType.isI32() || argType.isI64()) && gprsUsed != maxGPRs)
            WRITE_TO_METADATA(signatureMetadata + 2 * gprsUsed++, stackOffset, uint16_t);
        else if ((argType.isF32() || argType.isF64()) && fprsUsed != maxFPRs)
            WRITE_TO_METADATA(signatureMetadata + 16 + 2 * fprsUsed++, stackOffset, uint16_t);
        else
            locations.append(stackOffset);
    }

    // We can just leave the rest as 0. Doesn't matter what those register values are going into the function

    auto extraSize = roundUpToMultipleOf(8, locations.size() * 2 + 6);
    size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(extraSize);
    auto extraMetadata = m_metadata->m_metadata.data() + size;
    WRITE_TO_METADATA(extraMetadata, extraSize, uint16_t);
    WRITE_TO_METADATA(extraMetadata + 2, signature.argumentCount(), uint16_t);
    WRITE_TO_METADATA(extraMetadata + 4, locations.size(), uint16_t);
    for (size_t i = 0; i < locations.size(); ++i)
        WRITE_TO_METADATA(extraMetadata + 6 + i * 2, locations[i], uint16_t);

    // Returns
    Vector<Type> returns(signature.returnCount());
    for (size_t i = 0; i < signature.returnCount(); ++i)
        returns[i] = signature.returnType(i);
    m_metadata->addReturnData(returns);
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCall(uint32_t index, const TypeDefinition& type, Vector<ExpressionType>&, ResultList& results, CallType)
{
    const FunctionSignature& signature = *type.as<FunctionSignature>();
    for (unsigned i = 0; i < signature.returnCount(); i ++)
        results.append(Value { });

    // Function index:
    // 4B for decoded index
    // 4B for new PC
    auto newPC = m_parser->offset() - m_metadata->m_bytecodeOffset;
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(8);
    auto functionIndexMetadata = m_metadata->m_metadata.data() + size;
    WRITE_TO_METADATA(functionIndexMetadata, index, uint32_t);
    WRITE_TO_METADATA(functionIndexMetadata + 4, newPC, uint32_t);
    addCallCommonData(signature);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCallIndirect(unsigned tableIndex, const TypeDefinition& type, Vector<ExpressionType>&, ResultList& results, CallType)
{
    const FunctionSignature& signature = *type.as<FunctionSignature>();
    for (unsigned i = 0; i < signature.returnCount(); i ++)
        results.append(Value { });

    // Function index:
    // 4B for table index
    // 4B for type index
    auto newPC = m_parser->offset() - m_metadata->m_bytecodeOffset;
    auto size = m_metadata->m_metadata.size();
    m_metadata->addBlankSpace(16);
    auto functionIndexMetadata = m_metadata->m_metadata.data() + size;
    WRITE_TO_METADATA(functionIndexMetadata, tableIndex, uint32_t);
    WRITE_TO_METADATA(functionIndexMetadata + 4, m_metadata->addSignature(type), uint32_t);

    // 4B empty
    // 4B for PC
    WRITE_TO_METADATA(functionIndexMetadata + 12, newPC, uint32_t);

    addCallCommonData(signature);
    return { };
}

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCallRef(const TypeDefinition& type, Vector<ExpressionType>&, ResultList& results)
{
    const FunctionSignature& signature = *type.as<FunctionSignature>();
    for (unsigned i = 0; i < signature.returnCount(); i ++)
        results.append(Value { });
    return { };
}

// Traps

// Implementation status: UNIMPLEMENTED

PartialResult WARN_UNUSED_RETURN IPIntGenerator::addUnreachable() { return { }; }
PartialResult WARN_UNUSED_RETURN IPIntGenerator::addCrash() { return { }; }

// Finalize

std::unique_ptr<FunctionIPIntMetadataGenerator> IPIntGenerator::finalize()
{
    return WTFMove(m_metadata);
}

Expected<std::unique_ptr<FunctionIPIntMetadataGenerator>, String> parseAndCompileMetadata(const uint8_t* functionStart, size_t functionLength, const TypeDefinition& signature, ModuleInformation& info, uint32_t functionIndex)
{
    IPIntGenerator generator(info, functionIndex, signature, functionStart, functionLength);
    FunctionParser<IPIntGenerator> parser(generator, functionStart, functionLength, signature, info);
    WASM_FAIL_IF_HELPER_FAILS(parser.parse());
    return generator.finalize();
}


} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)
