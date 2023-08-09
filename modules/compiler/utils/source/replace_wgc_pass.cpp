// Copyright (C) Codeplay Software Limited
//
// Licensed under the Apache License, Version 2.0 (the "License") with LLVM
// Exceptions; you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://github.com/codeplaysoftware/oneapi-construction-kit/blob/main/LICENSE.txt
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// @file
///
/// @brief Defines the work-group collective builtins.

#include <compiler/utils/address_spaces.h>
#include <compiler/utils/attributes.h>
#include <compiler/utils/builtin_info.h>
#include <compiler/utils/dma.h>
#include <compiler/utils/group_collective_helpers.h>
#include <compiler/utils/mangling.h>
#include <compiler/utils/metadata.h>
#include <compiler/utils/pass_functions.h>
#include <compiler/utils/replace_wgc_pass.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ErrorHandling.h>
#include <multi_llvm/creation_apis_helper.h>
#include <multi_llvm/multi_llvm.h>

using namespace llvm;

namespace {

/// @brief Helper function that inserts a local barrier call via a builder.
///
/// @param[in] Builder ir builder used to create the barrier call. This function
/// will create the call at the current insert point of the builder.
CallInst *createLocalBarrierCall(IRBuilder<> &Builder,
                                 compiler::utils::BuiltinInfo &BI) {
  auto *const M = Builder.GetInsertBlock()->getModule();
  auto *const Barrier = BI.getOrDeclareMuxBuiltin(
      compiler::utils::eMuxBuiltinWorkGroupBarrier, *M);
  assert(Barrier && "__mux_work_group_barrier is not in module");

  auto *ID = Builder.getInt32(0);
  auto *Scope =
      Builder.getInt32(compiler::utils::BIMuxInfoConcept::MemScopeWorkGroup);
  auto *Semantics = Builder.getInt32(
      compiler::utils::BIMuxInfoConcept::MemSemanticsSequentiallyConsistent |
      compiler::utils::BIMuxInfoConcept::MemSemanticsWorkGroupMemory);
  auto *const BarrierCall =
      CallInst::Create(Barrier->getFunctionType(), Barrier,
                       {ID, Scope, Semantics}, "", Builder.GetInsertBlock());
  return BarrierCall;
}

Value *createSubgroupScan(IRBuilder<> &Builder, llvm::Value *Src,
                          RecurKind Kind, bool IsInclusive, bool IsLogical) {
  StringRef name;
  compiler::utils::TypeQualifier Q = compiler::utils::eTypeQualNone;
  switch (Kind) {
    default:
      return nullptr;
    case RecurKind::FAdd:
    case RecurKind::Add:
      name = IsInclusive ? StringRef("sub_group_scan_inclusive_add")
                         : StringRef("sub_group_scan_exclusive_add");
      break;
    case RecurKind::SMin:
      Q = compiler::utils::eTypeQualSignedInt;
      LLVM_FALLTHROUGH;
    case RecurKind::UMin:
    case RecurKind::FMin:
      name = IsInclusive ? StringRef("sub_group_scan_inclusive_min")
                         : StringRef("sub_group_scan_exclusive_min");
      break;
    case RecurKind::SMax:
      Q = compiler::utils::eTypeQualSignedInt;
      LLVM_FALLTHROUGH;
    case RecurKind::UMax:
    case RecurKind::FMax:
      name = IsInclusive ? StringRef("sub_group_scan_inclusive_max")
                         : StringRef("sub_group_scan_exclusive_max");
      break;
    case RecurKind::Mul:
    case RecurKind::FMul:
      name = IsInclusive ? StringRef("sub_group_scan_inclusive_mul")
                         : StringRef("sub_group_scan_exclusive_mul");
      break;
    case RecurKind::And:
      if (!IsLogical) {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_and")
                           : StringRef("sub_group_scan_exclusive_and");
      } else {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_logical_and")
                           : StringRef("sub_group_scan_exclusive_logical_and");
      }
      break;
    case RecurKind::Or:
      if (!IsLogical) {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_or")
                           : StringRef("sub_group_scan_exclusive_or");
      } else {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_logical_or")
                           : StringRef("sub_group_scan_exclusive_logical_or");
      }
      break;
    case RecurKind::Xor:
      if (!IsLogical) {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_xor")
                           : StringRef("sub_group_scan_exclusive_xor");
      } else {
        name = IsInclusive ? StringRef("sub_group_scan_inclusive_logical_xor")
                           : StringRef("sub_group_scan_exclusive_logical_xor");
      }
      break;
  }

  auto *const Ty = Src->getType();
  auto *const M = Builder.GetInsertBlock()->getModule();

  // Mangle the function name and look it up in the module.
  compiler::utils::NameMangler Mangler(&M->getContext());
  std::string MangledName = Mangler.mangleName(name, {Ty}, {Q});
  Function *Builtin = M->getFunction(MangledName);

  // Declare the builtin if necessary.
  if (!Builtin) {
    FunctionType *FT = FunctionType::get(Ty, {Ty}, false);
    M->getOrInsertFunction(MangledName, FT);
    Builtin = M->getFunction(MangledName);
    Builtin->setCallingConv(CallingConv::SPIR_FUNC);
  }

  return Builder.CreateCall(Builtin, {Src}, "wgc_scan");
}

/// @brief Helper function to create get subgroup size calls.
Value *createGetSubgroupSize(IRBuilder<> &Builder,
                             const Twine &Name = Twine()) {
  auto &M = *Builder.GetInsertBlock()->getModule();
  auto *const Builtin = cast<Function>(
      M.getOrInsertFunction(
           "_Z18get_sub_group_sizev",
           FunctionType::get(Builder.getInt32Ty(), /*isVarArg*/ false))
          .getCallee());
  assert(Builtin && "get_sub_group_size is not in module");

  Builtin->setCallingConv(CallingConv::SPIR_FUNC);
  return Builder.CreateCall(Builtin, {}, Name);
}

/// @brief Helper function to create subgroup broadcast calls.
Value *createSubgroupBroadcast(IRBuilder<> &Builder, Value *Src, Value *ID,
                               const Twine &Name = Twine()) {
  StringRef BuiltinName = "sub_group_broadcast";
  compiler::utils::TypeQualifier Q = compiler::utils::eTypeQualNone;

  auto *const Ty = Src->getType();
  auto *const M = Builder.GetInsertBlock()->getModule();
  auto &Ctx = M->getContext();

  auto *const Int32Ty = Type::getInt32Ty(Ctx);

  // Mangle the function name and look it up in the module.
  compiler::utils::NameMangler Mangler(&Ctx);
  std::string MangledName = Mangler.mangleName(
      BuiltinName, {Ty, Int32Ty}, {Q, compiler::utils::eTypeQualNone});
  Function *Builtin = M->getFunction(MangledName);

  // Declare the builtin if necessary.
  if (!Builtin) {
    FunctionType *FT = FunctionType::get(Ty, {Ty, Int32Ty}, false);
    M->getOrInsertFunction(MangledName, FT);
    Builtin = M->getFunction(MangledName);
    Builtin->setCallingConv(CallingConv::SPIR_FUNC);
  }

  return Builder.CreateCall(Builtin, {Src, ID}, Name);
}

/// @brief Helper function to get-or-create get_sub_group_local_id.
///
/// @param[in] M Module to create the function in.
///
/// @return The builtin.
Function *getOrCreateGetSubGroupLocalID(Module &M) {
  auto *const Int32Ty = Type::getInt32Ty(M.getContext());
  auto *const GetSubGroupLocalIDTy =
      FunctionType::get(Int32Ty, /*isVarArg*/ false);

  auto *const GetSubGroupLocalID = cast<Function>(
      M.getOrInsertFunction("_Z22get_sub_group_local_idv", GetSubGroupLocalIDTy)
          .getCallee());

  assert(GetSubGroupLocalID && "get_sub_group_local_id is not in module");
  GetSubGroupLocalID->setCallingConv(CallingConv::SPIR_FUNC);
  return GetSubGroupLocalID;
}

/// @brief Helper function to emit the binary op on the global accumulator
///
/// @param[in] Builder IR builder to build the operation with.
/// @param[in] CurrentVal The global accumulator which forms the lhs of the
/// binary operation.
/// @param[in] Operand The rhs of the binary operation.
/// @param[in] Kind The operation kind.
///
/// @return The result of the operation.
Value *createBinOp(llvm::IRBuilder<> &Builder, llvm::Value *CurrentVal,
                   llvm::Value *Operand, RecurKind Kind, bool IsAnyAll) {
  /// The semantics of bitwise "and" don't quite match the semantics of "all"
  /// (bitwise and isn't equivalent to logical and in a boolean context e.g.
  /// 01 & 10 = 00 but both 1 (01) and 2 (10) would be considered "true"), so
  /// for the sub_group_all reduction we need to work around this by emitting a
  /// few extra instructions.
  if (IsAnyAll && Kind == RecurKind::And) {
    auto *const IntType = Operand->getType();
    Value *Cmp = Builder.CreateICmpNE(Operand, ConstantInt::get(IntType, 0));
    Cmp = Builder.CreateIntCast(Cmp, IntType, /* isSigned */ true);
    return Builder.CreateAnd(Cmp, CurrentVal);
  }
  // Otherwise we can just use a single binary op.
  return multi_llvm::createBinOpForRecurKind(Builder, CurrentVal, Operand,
                                             Kind);
}

/// @brief Helper function to define the work-group collective scans.
///
/// param[in] F work-group collective scan to define, must be one of
/// work_group_exclusive_scan_add, work_group_exclusive_scan_min,
/// work_group_exclusive_scan_max, work_group_inclusive_scan_add,
/// work_group_inclusive_scan_min, work_group_inclusive_scan_max.
///
///
/// In terms of CL C this function defines a work-group inclusive scan as
/// follows:
///
/// local T accumulator;
/// T work_group_<inclusive:exclusive>_scan_<op>(T x) {
///    barrier(CLK_LOCAL_MEM_FENCE);  // Schedule = Once
///    accumulator = I;
///    barrier(CLK_LOCAL_MEM_FENCE);  // Schedule = Linear
///
///    T scan = sub_group_scan_<op>(x);
///    T result = accumulator + scan;
///
///    uint last = get_sub_group_size() - 1;
///    T reduce = sub_group_broadcast(scan, last);
/// #if exclusive
///    reduce += sub_group_broadcast(x, last);
/// #endif
///    accumulator += reduce;
///    barrier(CLK_LOCAL_MEM_FENCE);
///
///    return result;
/// }
///
/// where I is the neutral value value for the operation <op> on type T.
/// For exclusive scans on FMin and FMax, there is an added complexity caused
/// by the zeroth element of a scan, which is +/-INFINITY, but the true neutral
/// value of these operations is NaN. Thus we have to replace the zeroth element
/// of the subgroup scan with Nan, and replace the zeroth element of the final
/// result with +/INFINITY. There is a similar situation for FAdd, where the
/// identity element is defined to be `0.0` but the true neutral value is
/// `-0.0`.
void emitWorkGroupScanBody(const compiler::utils::GroupCollective &WGC,
                           compiler::utils::BuiltinInfo &BI) {
  // Create a global variable to do the scan on.
  auto &F = *WGC.Func;
  auto *const Operand = F.getArg(0);
  auto *const ReductionType{Operand->getType()};
  auto *const ReductionNeutralValue{
      compiler::utils::getNeutralVal(WGC.Recurrence, WGC.Ty)};
  assert(ReductionNeutralValue && "Invalid neutral value");
  auto &M = *F.getParent();
  auto *const Accumulator =
      new GlobalVariable{M,
                         ReductionType,
                         /* isConstant */ false,
                         GlobalVariable::LinkageTypes::InternalLinkage,
                         UndefValue::get(ReductionType),
                         F.getName() + ".accumulator",
                         /* InsertBefore */ nullptr,
                         GlobalVariable::ThreadLocalMode::NotThreadLocal,
                         compiler::utils::AddressSpace::Local};

  const auto IsInclusive = F.getName().contains("inclusive");

  auto &Ctx = F.getContext();
  auto *const EntryBB = BasicBlock::Create(Ctx, "entry", &F);

  IRBuilder<> Builder{EntryBB};

  // We need two barriers to isolate the accumulator initialization.
  compiler::utils::setBarrierSchedule(*createLocalBarrierCall(Builder, BI),
                                      compiler::utils::BarrierSchedule::Once);

  // Initialize the accumulator.
  Builder.CreateStore(ReductionNeutralValue, Accumulator);

  // The scans are defined in Linear order, so we must create a Linear barrier.
  compiler::utils::setBarrierSchedule(*createLocalBarrierCall(Builder, BI),
                                      compiler::utils::BarrierSchedule::Linear);

  // Read the accumulator.
  auto *const CurrentVal =
      Builder.CreateLoad(ReductionType, Accumulator, "current.val");

  // Perform the subgroup scan operation and add it to the accumulator.
  auto *SubScan = createSubgroupScan(Builder, Operand, WGC.Recurrence,
                                     IsInclusive, WGC.IsLogical);
  assert(SubScan && "Invalid subgroup scan");

  bool const NeedsIdentityFix =
      !IsInclusive &&
      (WGC.Recurrence == RecurKind::FAdd || WGC.Recurrence == RecurKind::FMin ||
       WGC.Recurrence == RecurKind::FMax);

  // For FMin/FMax, we need to fix up the identity element on the zeroth
  // subgroup ID, because it will be +/-INFINITY, but we need it to be NaN.
  // Likewise for FAdd, the zeroth element is defined to be 0.0, but the true
  // neutral value is -0.0.
  if (NeedsIdentityFix) {
    auto *const GetSubGroupLocalID = getOrCreateGetSubGroupLocalID(M);
    auto *const SubGroupLocalID =
        Builder.CreateCall(GetSubGroupLocalID, {}, "subgroup.id");
    auto *const IsZero =
        Builder.CreateICmp(CmpInst::ICMP_EQ, SubGroupLocalID,
                           ConstantInt::get(SubGroupLocalID->getType(), 0));
    SubScan = Builder.CreateSelect(IsZero, ReductionNeutralValue, SubScan);
  }

  auto *const Result =
      createBinOp(Builder, CurrentVal, SubScan, WGC.Recurrence, WGC.isAnyAll());

  // Update the accumulator with the last element of the subgroup scan
  auto *const LastElement = Builder.CreateNUWSub(
      createGetSubgroupSize(Builder, "wgc_sg_size"), Builder.getInt32(1));
  auto *const LastValue = createSubgroupBroadcast(Builder, SubScan, LastElement,
                                                  "wgc_sg_scan_tail");
  auto *SubReduce = LastValue;

  // If it's an exclusive scan, we have to add on the last element of the source
  // as well.
  if (!IsInclusive) {
    auto *const LastSrcValue =
        createSubgroupBroadcast(Builder, Operand, LastElement, "wgc_sg_tail");
    SubReduce = createBinOp(Builder, LastValue, LastSrcValue, WGC.Recurrence,
                            WGC.isAnyAll());
  }
  auto *const NextVal = createBinOp(Builder, CurrentVal, SubReduce,
                                    WGC.Recurrence, WGC.isAnyAll());
  Builder.CreateStore(NextVal, Accumulator);

  // A third barrier ensures that if there are two or more scans, they can't get
  // tangled up.
  createLocalBarrierCall(Builder, BI);

  if (NeedsIdentityFix) {
    auto *const Identity =
        compiler::utils::getIdentityVal(WGC.Recurrence, WGC.Ty);
    auto *const getLocalIDFn =
        BI.getOrDeclareMuxBuiltin(compiler::utils::eMuxBuiltinGetLocalId, M);
    auto *const IsZero = compiler::utils::isThreadZero(EntryBB, *getLocalIDFn);
    auto *const FixedResult = Builder.CreateSelect(IsZero, Identity, Result);
    Builder.CreateRet(FixedResult);
  } else {
    Builder.CreateRet(Result);
  }
}

/// @brief Defines the work-group collective functions.
///
/// @param[in] WGC Work-group collective function to be defined.
void emitWorkGroupCollectiveBody(const compiler::utils::GroupCollective &WGC,
                                 compiler::utils::BuiltinInfo &BI) {
  switch (WGC.Op) {
    case compiler::utils::GroupCollective::OpKind::All:
    case compiler::utils::GroupCollective::OpKind::Any:
    case compiler::utils::GroupCollective::OpKind::Reduction:
    case compiler::utils::GroupCollective::OpKind::Broadcast:
      // Do nothing. These are dealt with by the Handle Barriers Pass.
      break;
    case compiler::utils::GroupCollective::OpKind::ScanExclusive:
    case compiler::utils::GroupCollective::OpKind::ScanInclusive:
      emitWorkGroupScanBody(WGC, BI);
      break;
    default:
      llvm_unreachable("unhandled work-group collective");
  }
}
}  // namespace

PreservedAnalyses compiler::utils::ReplaceWGCPass::run(
    Module &M, ModuleAnalysisManager &AM) {
  // Only run this pass on OpenCL 2.0+ modules.
  auto Version = getOpenCLVersion(M);
  if (Version < OpenCLC20) {
    return PreservedAnalyses::all();
  }

  auto &BI = AM.getResult<BuiltinInfoAnalysis>(M);
  // This pass may insert new builtins into the module e.g. local barriers, so
  // we need to create a work-list before doing any work to avoid invalidating
  // iterators.
  SmallVector<GroupCollective, 8> WGCollectives{};
  for (auto &F : M) {
    auto WGC = isGroupCollective(&F);
    if (WGC && WGC->Scope == GroupCollective::ScopeKind::WorkGroup) {
      WGCollectives.push_back(*WGC);
    }
  }

  for (auto const WGC : WGCollectives) {
    emitWorkGroupCollectiveBody(WGC, BI);
  }
  return !WGCollectives.empty() ? PreservedAnalyses::none()
                                : PreservedAnalyses::all();
}
