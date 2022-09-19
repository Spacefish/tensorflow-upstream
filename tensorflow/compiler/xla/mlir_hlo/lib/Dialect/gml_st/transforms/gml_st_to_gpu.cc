/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <array>
#include <iterator>
#include <memory>
#include <utility>

#include "mlir-hlo/Dialect/gml_st/IR/gml_st_ops.h"
#include "mlir-hlo/Dialect/gml_st/transforms/passes.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"

#define GEN_PASS_DEF_GMLSTTOGPUPASS
#include "mlir-hlo/Dialect/gml_st/transforms/passes.h.inc"

using namespace mlir;
using namespace mlir::gml_st;
using mlir::gpu::LaunchOp;

namespace {
/// Converts a sequence of 3 nested gml_st.parallel ops into a gpu.launch op.
/// Throughout thes pass we will call the first level of nesting "block", the
/// second "warp", and the 3rd "thread" level. The intention is to allude to the
/// fact that these will likely correspond to the CUDA programming concepts of
/// the same name when the IR is lowered to PTX. However, this pass does not
/// make, nor verify all the requirements (e.g., that the warp-level iteration
/// contains exactly 32 steps) for mapping to this level.
///
/// Each gml_st.parallel is expected to only have a single induction variable.
/// The loops representing the block, warp, and thread level are mapped to
/// gridDim.x, blockDim.y, and blockDim.x launch dimensions of gpu.launch,
/// respectively.
///
/// All operations from within the nested gml_st.parallel regions are copied
/// directly into the gpu.launch region, with induction variables replaced by
/// equivalent values computed using the blockIdx.x, threadIdx.y and threadIdx.x
/// indices. Thus, the 3 nested parallel regions are effectively flattened into
/// a single level of nesting within the gpu.launch region.
///
/// At any level of nesting, multiple gml_st.parallel operations are allowed, as
/// long as they have the same iteration space, i.e., the SSA values defining
/// the lower bound, upper bound and the step of all parallels on the same level
/// of nesting are the same values.
struct ParallelOpToGpuPattern : public OpRewritePattern<ParallelOp> {
  using OpRewritePattern<ParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ParallelOp root,
                                PatternRewriter& rewriter) const override;
};

/// Implements the GmlStToGpuPass declared in
/// include/mlir-hlo/Dialect/gml_st/transforms/passes.td.
struct GmlStToGpuPass : public ::impl::GmlStToGpuPassBase<GmlStToGpuPass> {
  void runOnOperation() override {
    RewritePatternSet patterns(&getContext());
    patterns.add<ParallelOpToGpuPattern>(patterns.getContext());
    ConversionTarget target(getContext());
    target.addIllegalDialect<GmlStDialect>();
    // We're producing new ops (clones of original ops in gml_st.parallel
    // loops), so we have to mark them explicitly legal, otherwise the
    // conversion fails even if doing partial conversion.
    target.markUnknownOpDynamicallyLegal([](Operation*) { return true; });
    if (failed(
            applyFullConversion(getOperation(), target, std::move(patterns))))
      signalPassFailure();
  }
};
}  // namespace

/// Creates an initial gpu.launch op with launch configuration set to a single
/// thread. The idea is to update those later, as we discover the correct values
/// from the nesting structure.
static LaunchOp createInitialGpuLaunchOp(Location loc, Value defaultSize,
                                         PatternRewriter& rewriter) {
  auto launch =
      rewriter.create<LaunchOp>(loc, defaultSize, defaultSize, defaultSize,
                                defaultSize, defaultSize, defaultSize);
  Block* body = &launch.body().front();
  rewriter.setInsertionPointToEnd(body);
  rewriter.create<gpu::TerminatorOp>(loc);
  rewriter.setInsertionPointToStart(body);
  return launch;
}

/// Returns the induction variable index `idx` of gpu.launch that should be used
/// for the given gml_st.parallel's `nestingLevel`.
static LogicalResult getInductionVarIdxForLevel(unsigned nestingLevel,
                                                unsigned& idx) {
  constexpr std::array<unsigned, 3> kNestingToLaunchIdx{
      0,  // block IDs map to blockIdx.x
      4,  // warp IDs map to threadIdx.y
      3,  // thread IDs map to threadIdx.x
  };
  if (nestingLevel >= kNestingToLaunchIdx.size()) return failure();
  idx = kNestingToLaunchIdx[nestingLevel];
  return success();
}

/// Verifies that the loop bounds of `currentBound` (which is a result of
/// affine.apply) are the same ones as the bounds of the `parallel` op.
static LogicalResult verifyLoopBoundsMatch(Value currentBound,
                                           ParallelOp parallel) {
  auto applyOp = currentBound.getDefiningOp<AffineApplyOp>();
  assert(applyOp && "inferred bounds should be expressed as affine.apply");
  OperandRange operands = applyOp.getMapOperands();
  assert(operands.size() == 3 &&
         "affine map expressing the launch bound should have three operands");
  return success(operands[0] == parallel.getUpperBound().front() &&
                 operands[1] == parallel.getLowerBound().front() &&
                 operands[2] == parallel.getStep().front());
}

/// Matches the `launchIdx`-th iteration space of `launch` to the iteration
/// space of `parallel`. Returns an SSA value that is a part of the `launch`'s
/// region, and represents the value of `parallel`'s induction variable.
static Value matchLaunchSpaceToLoop(ParallelOp parallel, LaunchOp launch,
                                    unsigned launchIdx,
                                    PatternRewriter& rewriter) {
  Location loc = parallel.getLoc();
  Value upperBound = parallel.getUpperBound().front();
  Value lowerBound = parallel.getLowerBound().front();
  Value step = parallel.getStep().front();

  // Compute the value that gml_st.parallel's induction variable would have in
  // each iteration, and make it available to operations within the gpu.launch
  // region.
  AffineMap inductionVarMap = AffineMap::get(
      /*dimCount=*/1, /*symbolCount=*/2,
      rewriter.getAffineDimExpr(0) * rewriter.getAffineSymbolExpr(1) +
          rewriter.getAffineSymbolExpr(0));
  Value inductionVar = rewriter.create<AffineApplyOp>(
      loc, inductionVarMap,
      ValueRange{launch.body().getArgument(launchIdx), lowerBound, step});

  // Infer the launch bound from the loop bounds and the step.
  AffineMap launchBoundMap = AffineMap::get(
      /*dimCount=*/1, /*symbolCount=*/2,
      (rewriter.getAffineDimExpr(0) - rewriter.getAffineSymbolExpr(0))
          .ceilDiv(rewriter.getAffineSymbolExpr(1)));
  PatternRewriter::InsertionGuard guard(rewriter);
  rewriter.setInsertionPoint(launch);
  launch.setOperand(launchIdx, rewriter.create<AffineApplyOp>(
                                   loc, launchBoundMap,
                                   ValueRange{upperBound, lowerBound, step}));
  return inductionVar;
}

// Converts the 3 nested gml_st.parallel ops rooted at `root` into a
// gpu.launch op. We do this by creating an empty gpu.launch region and
// copying all the operations in gml_st.parallel into that region,
// recursively copying the bodies of any nested gml_st.parallel regions that
// we encounter.
LogicalResult ParallelOpToGpuPattern::matchAndRewrite(
    ParallelOp root, PatternRewriter& rewriter) const {
  Location loc = root.getLoc();

  Value defaultSize = rewriter.create<arith::ConstantIndexOp>(loc, 1);
  LaunchOp launch = createInitialGpuLaunchOp(loc, defaultSize, rewriter);

  BlockAndValueMapping bvm;
  // We need to keep track of which value in the gpu.launch region represents
  // which level of the induction variable in the nested region. This is because
  // we might have multiple gml_st.parallel operations on the same level, and
  // their induction variables should map to the same value in the flattened
  // gpu.launch region.
  SmallVector<Value, 3> nestingLevelToInductionVarMap;
  // This is our stack holding in-flight operations of gml_st.parallel regions
  // that we started to copy over to the gpu.launch region, but are on hold
  // while we are processing a nested gml_st.parallel.
  SmallVector<iterator_range<Block::iterator>, 3> loopIterators;

  // This functor implements the processing of a single parallel op:
  // 1)  update of GPU launch bounds according to the interation space
  // 2)  addition of a nesting level to `loopIterators`, with the iterator
  //     over `parallel`'s body
  auto processParallelOp = [&](ParallelOp parallel) {
    unsigned nestingLevel = loopIterators.size();
    unsigned inductionVarIdx = 0;
    if (failed(getInductionVarIdxForLevel(nestingLevel, inductionVarIdx)))
      return rewriter.notifyMatchFailure(parallel, "is nested too deeply");
    if (parallel.getNumLoops() != 1) {
      return rewriter.notifyMatchFailure(
          parallel, "should only have a single induction variable");
    }

    Value currentBound = launch.getOperand(inductionVarIdx);
    if (nestingLevel < nestingLevelToInductionVarMap.size()) {
      // We already inferred the launch bound for this nesting level.
      if (failed(verifyLoopBoundsMatch(currentBound, parallel))) {
        return rewriter.notifyMatchFailure(
            parallel,
            "should have the same iteration space as other parallel operations "
            "on the same nesting level");
      }
    } else {
      // We are encountering a loop at this level of nesting for the first time.
      assert(currentBound == defaultSize &&
             "launch bound should use the default size");
      nestingLevelToInductionVarMap.push_back(
          matchLaunchSpaceToLoop(parallel, launch, inductionVarIdx, rewriter));
    }

    bvm.map(parallel.getInductionVars().front(),
            nestingLevelToInductionVarMap[nestingLevel]);
    loopIterators.push_back(parallel.getBody()->without_terminator());
    return success();
  };

  if (failed(processParallelOp(root))) return failure();

  while (!loopIterators.empty()) {
    auto currentLoop = loopIterators.pop_back_val();
    for (Operation& op : currentLoop) {
      if (auto nestedParallel = dyn_cast<ParallelOp>(&op)) {
        // Push the current state back to loopIterator and start the next level
        // of nesting.
        loopIterators.push_back(
            llvm::make_range(std::next(op.getIterator()), currentLoop.end()));
        if (failed(processParallelOp(nestedParallel))) return failure();
        break;
      }
      // TODO(b/244314146): Figure out what we need to do for operations
      // encountered on upper nesting levels to correctly lower them after the
      // rewrite to gpu.launch.
      Operation* clone = rewriter.clone(op, bvm);
      bvm.map(op.getResults(), clone->getResults());
    }
  }

  rewriter.eraseOp(root);
  return success();
}
