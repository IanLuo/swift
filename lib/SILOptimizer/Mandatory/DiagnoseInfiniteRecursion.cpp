//==-- DiagnoseInfiniteRecursion.cpp - Find infinitely-recursive applies --==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements a diagnostic pass that detects infinite recursive
// function calls.
//
// It detects simple forms of infinite recursions, like
//
//   func f() {
//     f()
//   }
//
// and can also deal with invariant conditions, like availability checks
//
//   func f() {
//     if #available(macOS 10.4.4, *) {
//       f()
//     }
//   }
//
// or invariant conditions due to forwarded arguments:
//
//   func f(_ x: Int) {
//     if x > 0 {
//       f(x)
//     }
//   }
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "infinite-recursion"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Returns true if \p inst is a full-apply site which calls the containing
/// function.
static bool isRecursiveCall(FullApplySite applySite) {
  SILFunction *parentFunc = applySite.getFunction();
  if (SILFunction *calledFn = applySite.getReferencedFunctionOrNull())
    return calledFn == parentFunc;

  // Don't touch dynamic dispatch.
  const auto callee = applySite.getCallee();
  if (isa<SuperMethodInst>(callee) ||
      isa<ObjCSuperMethodInst>(callee) ||
      isa<ObjCMethodInst>(callee)) {
    return false;
  }

  if (auto *CMI = dyn_cast<ClassMethodInst>(callee)) {

    // FIXME: If we're not inside the module context of the method,
    // we may have to deserialize vtables.  If the serialized tables
    // are damaged, the pass will crash.
    //
    // Though, this has the added bonus of not looking into vtables
    // outside the current module.  Because we're not doing IPA, let
    // alone cross-module IPA, this is all well and good.
    SILModule &module = parentFunc->getModule();
    CanType classType = CMI->getOperand()->getType().getASTType();
    ClassDecl *classDecl = classType.getClassOrBoundGenericClass();
    if (classDecl && classDecl->getModuleContext() != module.getSwiftModule())
      return false;

    if (!calleesAreStaticallyKnowable(module, CMI->getMember()))
      return false;

    // The "statically knowable" check just means that we have all the
    // callee candidates available for analysis. We still need to check
    // if the current function has a known override point.
    auto *methodDecl = CMI->getMember().getAbstractFunctionDecl();
    if (methodDecl->isOverridden())
      return false;

    SILFunction *method = getTargetClassMethod(module, classDecl, CMI);
    return method == parentFunc;
  }

  if (auto *WMI = dyn_cast<WitnessMethodInst>(callee)) {
    auto funcAndTable = parentFunc->getModule().lookUpFunctionInWitnessTable(
        WMI->getConformance(), WMI->getMember());
    return funcAndTable.first == parentFunc;
  }
  return false;
}

/// For the purpose of this analysis we can exclude certain memory-writing
/// instructions.
static bool mayWriteToMemory(SILInstruction *inst) {
  switch (inst->getKind()) {
  case SILInstructionKind::LoadInst:
    // A `load` is defined to write memory or have side effects in two cases:
    // * We don't care about retain instructions of a `load [copy]`.
    // * We don't care about a `load [take]` because it cannot occur in an
    //   infinite recursion loop without another write (which re-initializes
    //   the memory).
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::EndAccessInst:
    return false;
  default:
    return inst->mayWriteToMemory();
  }
}

/// Describes what is expected to be invariant in an infinite recursion loop.
///
/// * Memory: it's all or nothing. Either all memory is expected to be invariant
///   (= never written) or not. We could use AliasAnalysis to do a more fine-
///   grained analysis, but in mandatory optimizations we want to keep things
///   simple.
///
/// * Arguments: an argument is invariant if a recursive call forwards the
///   incoming argument. For example:
///   \code
///     func f(_ x: Int, _ y: Int) {
///       f(x, y - 1) // The first argument is invariant, the second is not
///     }
///   \endcode
class Invariants {
  enum {
    /// The first bit represents invariant memory.
    invariantMemoryBit = 0,
    /// The remaining bits are used for arguments.
    firstArgBit = 1,
    maxArgIndex = 16 // should be more than enough.
  };

  static_assert((unsigned)(1 << (firstArgBit + maxArgIndex)) != 0,
                "too many argument bits");

  unsigned bitMask;

  explicit Invariants(unsigned bitMask) : bitMask(bitMask) { }

  bool isBitSet(int bitNr) const { return (bitMask & (1 << bitNr)) != 0; }

  /// Recursively walks the use-def chain starting at \p value and returns
  /// true if all visited values are invariant.
  bool isInvariantValue(SILValue value,
                        SmallPtrSetImpl<SILNode *> &visited) const {
    SILNode *node = value->getRepresentativeSILNodeInObject();

    // Avoid exponential complexity in case a value is used by multiple
    // operands.
    if (!visited.insert(node).second)
      return true;

    if (auto *inst = dyn_cast<SILInstruction>(node)) {
      if (!isMemoryInvariant() && inst->mayReadFromMemory())
        return false;

      for (Operand &op : inst->getAllOperands()) {
        if (!isInvariantValue(op.get(), visited))
          return false;
      }
      return true;
    }

    if (auto *funcArg = dyn_cast<SILFunctionArgument>(value)) {
      return isArgumentInvariant(funcArg->getIndex());
    }

    return false;
  }

  friend llvm::DenseMapInfo<Invariants>;

public:

  static Invariants noInvariants() { return Invariants(0); }

  /// Constructs invariants which include all forwarding arguments of
  /// \p recursiveApply.
  static Invariants fromForwardingArguments(FullApplySite recursiveApply) {
    unsigned bitMask = 0;
    auto incomingArgs = recursiveApply.getFunction()->getArguments();
    for (auto argAndIndex : llvm::enumerate(recursiveApply.getArguments())) {
      unsigned argIdx = argAndIndex.index();
      if (argIdx <= maxArgIndex &&
          stripAccessMarkers(argAndIndex.value()) == incomingArgs[argIdx])
        bitMask |= (1 << (argIdx + firstArgBit));
    }
    return Invariants(bitMask);
  }

  Invariants withInvariantMemory() const {
    return Invariants(bitMask | (1 << invariantMemoryBit));
  }

  bool isMemoryInvariant() const { return isBitSet(invariantMemoryBit); }

  bool isArgumentInvariant(unsigned argIdx) const {
    return argIdx <= maxArgIndex && isBitSet(argIdx + firstArgBit);
  }

  /// Returns true if \p term is a conditional terminator and has an invariant
  /// condition.
  bool isInvariant(TermInst *term) const {
    switch (term->getTermKind()) {
    case TermKind::SwitchEnumAddrInst:
    case TermKind::CheckedCastAddrBranchInst:
      if (!isMemoryInvariant())
        return false;
      LLVM_FALLTHROUGH;
    case TermKind::CondBranchInst:
    case TermKind::SwitchValueInst:
    case TermKind::SwitchEnumInst:
    case TermKind::CheckedCastBranchInst:
    case TermKind::CheckedCastValueBranchInst: {
      SmallPtrSet<SILNode *, 16> visited;
      return isInvariantValue(term->getOperand(0), visited);
    }
    default:
      return false;
    }
  }

  /// Returns true if \p recursiveApply is forwarding all arguments which are
  /// expected to be invariant.
  bool hasInvariantArguments(FullApplySite recursiveApply) const {
    auto incomingArgs = recursiveApply.getFunction()->getArguments();
    for (auto argAndIndex : llvm::enumerate(recursiveApply.getArguments())) {
      unsigned argIdx = argAndIndex.index();
      if (isArgumentInvariant(argIdx) &&
          stripAccessMarkers(argAndIndex.value()) != incomingArgs[argIdx]) {
        return false;
      }
    }
    return true;
  }
};

} // end anonymous namespace

namespace  llvm {
  template<> struct DenseMapInfo<Invariants> {
    static Invariants getEmptyKey() {
      return Invariants(DenseMapInfo<unsigned>::getEmptyKey());
    }
    static Invariants getTombstoneKey() {
      return Invariants(DenseMapInfo<unsigned>::getTombstoneKey());
    }
    static unsigned getHashValue(Invariants deps) {
      return DenseMapInfo<unsigned>::getHashValue(deps.bitMask);
    }
    static bool isEqual(Invariants LHS, Invariants RHS) {
      return LHS.bitMask == RHS.bitMask;
    }
  };
}

namespace {

/// Contains block-specific info which is needed to do the analysis.
struct BlockInfo {
  /// non-null if this block contains a recursive call.
  SILInstruction *recursiveCall;

  /// The number of successors which reach a `return`.
  unsigned numSuccsNotReachingReturn;

  /// True if the block has a terminator with an invariant condition.
  ///
  /// Note: "invariant" means: invariant with respect to the expected invariants,
  ///       which are passed to the constructor.
  bool hasInvariantCondition;

  /// Is there any path from the this block to a function return, without going
  /// through a recursive call?
  ///
  /// This flag is propagated up the control flow, starting at returns.
  ///
  /// Note that if memory is expected to be invariant, all memory-writing
  /// instructions are also considered as a "return".
  bool reachesReturn;

  /// Is there any path from the entry to this block without going through a
  /// `reachesReturn` block.
  ///
  /// This flag is propagated down the control flow, starting at entry. If this
  /// flag reaches a block with a recursiveCall, it means that it's an infinite
  /// recursive call.
  bool reachableFromEntry;

  // Make DenseMap<...,BlockInfo> compilable.
  BlockInfo() {
    llvm_unreachable("DenseMap should not construct an empty BlockInfo");
  }

  /// Get block information with expected \p invariants.
  BlockInfo(SILBasicBlock *block, Invariants invariants) :
      recursiveCall(nullptr),
      numSuccsNotReachingReturn(block->getNumSuccessors()),
      hasInvariantCondition(invariants.isInvariant(block->getTerminator())),
      reachesReturn(false), reachableFromEntry(false) {
    for (SILInstruction &inst : *block) {
      if (auto applySite = FullApplySite::isa(&inst)) {
        // Ignore blocks which call a @_semantics("programtermination_point").
        // This is an assert-like program termination and we explicitly don't
        // want this call to disqualify the warning for infinite recursion,
        // because they're reserved for exceptional circumstances.
        if (applySite.isCalleeKnownProgramTerminationPoint())
          return;

        if (isRecursiveCall(applySite) &&
            invariants.hasInvariantArguments(applySite)) {
          recursiveCall = &inst;
          return;
        }
      }
      if (invariants.isMemoryInvariant() && mayWriteToMemory(&inst)) {
        // If we are assuming that all memory is invariant, a memory-writing
        // instruction potentially breaks the infinite recursion loop. For the
        // sake of the anlaysis, it's like a function return.
        reachesReturn = true;
        return;
      }
    }
    TermInst *term = block->getTerminator();
    if (term->isFunctionExiting() ||
        // Also treat non-assert-like unreachables as returns, like "exit()".
        term->isProgramTerminating()) {
      reachesReturn = true;
    }
  }
};

/// Performs the analysis to detect infinite recursion loops.
///
/// The basic idea is to see if there is a path from the entry block to a
/// function return without going through an infinite recursive call.
///
/// The analysis is done with a given set of invariants (see Invariants). The
/// correctness of the result (i.e. no false infinite recursion reported) does
/// _not_ depend on the chosen invariants. But it's a trade-off:
/// The more invariants we include, the more conditions might become invariant
/// (which is good). On the other hand, we have to ignore recursive calls which
/// don't forward all invariant arguments.
///
/// We don't know in advance which invariants will yield the best result, i.e.
/// let us detect an infinite recursion.
/// For example, in f() we can only detect the infinite recursion if we expect
/// that the parameter `x` is invariant.
///
///   func f(_ x: Int) {
///     if x > 0 {   // an invariant condition!
///       f(x)       // the call is forwarding the argument
///     }
///   }
///
/// But in g() we can only detect the infinite recursion if we _don't_ expect
/// that the parameter is invariant.
///
///   func g(_ x: Int) {
///     if x > 0 {   // no invariant condition
///       g(x - 1)   // argument is not forwarded
///     } else {
///       g(x - 2)   // argument is not forwarded
///     }
///   }
///
class InfiniteRecursionAnalysis {
  SILFunction *function;
  llvm::DenseMap<SILBasicBlock *, BlockInfo> blockInfos;

  InfiniteRecursionAnalysis(SILFunction *function) :
    function(function),
    // Reserve enough space in the map. Though, SILFunction::size() iterates
    // over all blocks. But this is still better than to risk multiple mallocs.
    blockInfos(function->size()) { }

  BlockInfo &info(SILBasicBlock *block) { return blockInfos[block]; }

  /// Propagates the `reachesReturn` flags up the control flow and returns true
  /// if the flag reaches the entry block.
  bool isEntryReachableFromReturn(Invariants invariants) {
    // Contains blocks for which the `reachesReturn` flag is set.
    SmallVector<SILBasicBlock *, 32> workList;

    // First, initialize the block infos.
    for (SILBasicBlock &block : *function) {
      BlockInfo blockInfo(&block, invariants);
      blockInfos.insert({&block, blockInfo});
      if (blockInfo.reachesReturn)
        workList.push_back(&block);
    }

    while (!workList.empty()) {
      SILBasicBlock *block = workList.pop_back_val();
      for (auto *pred : block->getPredecessorBlocks()) {
        BlockInfo &predInfo = info(pred);
        if (predInfo.reachesReturn ||
            // Recursive calls block the flag propagation.
            predInfo.recursiveCall != nullptr)
          continue;

        assert(predInfo.numSuccsNotReachingReturn > 0);
        predInfo.numSuccsNotReachingReturn -= 1;

        // This is the trick for handling invariant conditions: usually the
        // `reachesReturn` flag is propagated if _any_ of the successors has it
        // set.
        // For invariant conditions, it's only propagated if _all_ successors
        // have it set. If at least one of the successors reaches a recursive
        // call and this successor is taken once, it will be taken forever
        // (because the condition is invariant).
        if (predInfo.hasInvariantCondition &&
            predInfo.numSuccsNotReachingReturn > 0)
          continue;

        predInfo.reachesReturn = true;
        workList.push_back(pred);
      }
    }
    return info(function->getEntryBlock()).reachesReturn;
  }

  /// Propagates the `reachableFromEntry` flags down the control flow and
  /// issues a warning if it reaches a recursive call.
  /// Returns true, if at least one recursive call is found.
  bool findRecursiveCallsAndDiagnose() {
    SmallVector<SILBasicBlock *, 32> workList;
    SILBasicBlock *entryBlock = function->getEntryBlock();
    info(entryBlock).reachableFromEntry = true;
    workList.push_back(entryBlock);

    bool foundInfiniteRecursion = false;
    while (!workList.empty()) {
      SILBasicBlock *block = workList.pop_back_val();
      if (auto *recursiveCall = info(block).recursiveCall) {
        function->getModule().getASTContext().Diags.diagnose(
                 recursiveCall->getLoc().getSourceLoc(),
                 diag::warn_infinite_recursive_call);
        foundInfiniteRecursion = true;
        continue;
      }
      for (auto *succ : block->getSuccessorBlocks()) {
        BlockInfo &succInfo = info(succ);
        if (!succInfo.reachesReturn && !succInfo.reachableFromEntry) {
          succInfo.reachableFromEntry = true;
          workList.push_back(succ);
        }
      }
    }
    return foundInfiniteRecursion;
  }

public:

  LLVM_ATTRIBUTE_USED void dump() {
    for (SILBasicBlock &block : *function) {
      BlockInfo &blockInfo = info(&block);
      llvm::dbgs() << "bb" << block.getDebugID()
                   << ": numSuccs= " << blockInfo.numSuccsNotReachingReturn;
      if (blockInfo.recursiveCall)
        llvm::dbgs() << " hasRecursiveCall";
      if (blockInfo.hasInvariantCondition)
        llvm::dbgs() << " hasInvariantCondition";
      if (blockInfo.reachesReturn)
        llvm::dbgs() << " reachesReturn";
      if (blockInfo.reachableFromEntry)
        llvm::dbgs() << " reachesRecursiveCall";
      llvm::dbgs() << '\n';
    }
  }

  /// Performs the analysis and issues a warnings for recursive calls.
  /// Returns true, if at least one recursive call is found.
  static bool analyzeAndDiagnose(SILFunction *function, Invariants invariants) {
    InfiniteRecursionAnalysis analysis(function);
    if (analysis.isEntryReachableFromReturn(invariants))
      return false;

    // Now we know that the function never returns.
    // There can be three cases:
    // 1. All paths end up in an abnormal program termination, like fatalError().
    //    We don't want to warn about this. It's probably intention.
    // 2. There is an infinite loop.
    //    We don't want to warn about this either. Maybe it's intention. Anyway,
    //    this case is handled by the DiagnoseUnreachable pass.
    // 3. There is an infinite recursion.
    //    That's what we are interested in. We do a forward propagation to find
    //    the actual infinite recursive call(s) - if any.
    return analysis.findRecursiveCallsAndDiagnose();
  }
};

typedef SmallSetVector<Invariants, 4> InvariantsSet;

/// Collect invariants with which we should try the analysis and return true if
/// there is at least one recursive call in the function.
static bool collectInvariantsToTry(SILFunction *function,
                                   InvariantsSet &invariantsToTry) {
  // Try with no invariants.
  invariantsToTry.insert(Invariants::noInvariants());

  bool recursiveCallsFound = false;

  // Scan the function for recursive calls.
  for (SILBasicBlock &block : *function) {
    for (auto &inst : block) {
      auto applySite = FullApplySite::isa(&inst);
      if (applySite && isRecursiveCall(applySite)) {
        recursiveCallsFound = true;

        // See what parameters the recursive call is forwarding and use that
        // as invariants.
        invariantsToTry.insert(Invariants::fromForwardingArguments(applySite));

        // Limit the size of the set to avoid quadratic complexity in corner
        // cases. Usually 4 invariants are more than enough.
        if (invariantsToTry.size() >= 4)
          return true;
      }
    }
  }
  return recursiveCallsFound;
}

class DiagnoseInfiniteRecursion : public SILFunctionTransform {
public:
  DiagnoseInfiniteRecursion() {}

private:
  void run() override {
    SILFunction *function = getFunction();
    // Don't rerun diagnostics on deserialized functions.
    if (function->wasDeserializedCanonical())
      return;

    // Try with different sets of invariants. To catch all cases we would need
    // to try all parameter/memory permutations.
    // But in practice, it's good enough to collect a reasonable set by finding
    // all recursive calls and see what arguments they are forwarding.
    InvariantsSet invariantsToTry;
    if (!collectInvariantsToTry(function, invariantsToTry)) {
      // There are no recursive calls in the function at all. We don't need to
      // ramp-up the analysis.
      // This is the case for most functions.
      return;
    }

    for (Invariants invariants : invariantsToTry) {
      if (InfiniteRecursionAnalysis::analyzeAndDiagnose(function, invariants))
        return;
      // Try again, assuming that memory is invariant.
      if (InfiniteRecursionAnalysis::analyzeAndDiagnose(
                               function, invariants.withInvariantMemory()))
        return;
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnoseInfiniteRecursion() {
  return new DiagnoseInfiniteRecursion();
}
