//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/Analysis/LoopAnalysis.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "scalehls/Dialect/HLS/Analysis.h"
#include "scalehls/Transforms/Passes.h"
#include "scalehls/Transforms/Utils.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "parallelize-dataflow-node"

using namespace mlir;
using namespace scalehls;

/// Apply loop vectorization to the loop band.
static bool applyLoopVectorization(AffineLoopBand &band,
                                   FactorList vectorFactors) {
  assert(!band.empty() && "no loops provided");
  if (llvm::all_of(vectorFactors, [](unsigned factor) { return factor == 1; }))
    return true;

  // We require all loops to be parallel loop.
  for (auto loop : band)
    if (!(hasParallelAttr(loop) || isLoopParallel(loop)))
      return false;

  // Apply loop vectorization.
  auto loopSet = llvm::DenseSet<Operation *>(band.begin(), band.end());
  auto sizes = SmallVector<int64_t>(vectorFactors.begin(), vectorFactors.end());
  vectorizeAffineLoops(band.front()->getParentOp(), loopSet, sizes, {});
  return true;
}

namespace {
struct ParallelizeDataflowNode
    : public ParallelizeDataflowNodeBase<ParallelizeDataflowNode> {
  ParallelizeDataflowNode() = default;
  ParallelizeDataflowNode(unsigned loopUnrollFactor, bool unrollPointLoopOnly,
                          bool argComplexityAware, bool argCorrelationAware) {
    maxUnrollFactor = loopUnrollFactor;
    pointLoopOnly = unrollPointLoopOnly;
    complexityAware = argComplexityAware;
    correlationAware = argCorrelationAware;
  }

  /// Try to calculate the unroll factors of the nodes contained in each
  /// dataflow schedule.
  void getNodeParallelFactorMap(func::FuncOp func) {
    auto compAnal = ComplexityAnalysis(func);
    nodeParallelFactorMap.clear();

    func.walk<WalkOrder::PreOrder>([&](ScheduleOp schedule) {
      unsigned long scheduleUnrollFactor = maxUnrollFactor.getValue();
      if (auto parentNode = schedule->getParentOfType<NodeOp>()) {
        if (!nodeParallelFactorMap.count(parentNode)) {
          parentNode.emitOpError("failed to get parent node's unroll factor");
          return WalkResult::interrupt();
        }
        scheduleUnrollFactor = nodeParallelFactorMap.lookup(parentNode);
        // FIXME: A hacky method to hand tune the factors and resolve
        // outstanding dataflow nodes.
        if (auto attr = schedule->getAttr("increase"))
          if (auto annoFactor = attr.dyn_cast<IntegerAttr>())
            scheduleUnrollFactor *= annoFactor.getInt();
        if (auto attr = schedule->getAttr("decrease"))
          if (auto annoFactor = attr.dyn_cast<IntegerAttr>())
            scheduleUnrollFactor /= annoFactor.getInt();
      }

      auto scheduleComplexity = compAnal.getScheduleComplexity(schedule);
      if (!scheduleComplexity.has_value()) {
        schedule.emitOpError("failed to get schedule complexity");
        return WalkResult::interrupt();
      }

      for (auto node : schedule.getOps<NodeOp>()) {
        auto nodeComplexity = compAnal.getNodeComplexity(node);
        if (!nodeComplexity.has_value()) {
          node.emitOpError("failed to get node complexity");
          return WalkResult::interrupt();
        }
        auto nodeUnrollFactor = std::max(
            (unsigned long)1, scheduleUnrollFactor * nodeComplexity.value() /
                                  scheduleComplexity.value());
        nodeParallelFactorMap.insert({node, nodeUnrollFactor});

        LLVM_DEBUG(
            // clang-format off
          llvm::dbgs() << "\nNode Complexity: " << nodeComplexity.value() << "\n";
          llvm::dbgs() << "Schedule Complexity: " << scheduleComplexity.value() << "\n";
          llvm::dbgs() << "Node Factor: " << nodeUnrollFactor << "\n";
          llvm::dbgs() << "Schedule Factor: " << scheduleUnrollFactor << "\n";
          llvm::dbgs() << "Node at " << node.getLoc() << ": \n" << node << "\n";
            // clang-format on
        );
      }
      return WalkResult::advance();
    });
  }

  /// Unroll dataflow node with the given parallel factor. If the pass is not
  /// complexity aware, always unroll with the max unroll factor.
  void applyNaiveLoopUnroll(NodeOp node, unsigned parallelFactor) {
    auto unrollFactor = parallelFactor;
    if (!complexityAware)
      unrollFactor = maxUnrollFactor.getValue();

    // Collect all loop bands to be unrolled.
    AffineLoopBands bands;
    node.walk([&](AffineForOp loop) {
      if (loop->getParentOfType<NodeOp>() == node &&
          loop.getOps<mlir::AffineForOp>().empty() &&
          loop.getOps<ScheduleOp>().empty()) {
        AffineLoopBand band;
        getLoopBandFromInnermost(loop, band);
        bands.push_back(band);
      }
    });

    for (auto &band : bands) {
      // For loop band that has effect on external buffers, we should directly
      // unroll them without considering whether it's point loop.
      // FIXME: Need a better solution on handling external buffers.
      if (pointLoopOnly && !hasEffectOnExternalBuffer(band.front())) {
        AffineLoopBand tileBand;
        AffineLoopBand pointBand;
        if (!getTileAndPointLoopBand(band, tileBand, pointBand) ||
            pointBand.empty())
          continue;
        band = pointBand;
      }

      auto factors = FactorList(band.size(), 1);
      if (failed(getEvenlyDistributedFactors(unrollFactor, factors, band)))
        factors = getDistributedFactors(unrollFactor, band);
      applyLoopUnrollJam(band, factors);
    }
  }

  /// Unroll loops based on the correlations between dataflow nodes.
  void applyCorrelationAwareUnroll(func::FuncOp func) {
    auto corrAnal = CorrelationAnalysis(func);

    // We first sort all nodes in a decending order of their associated number
    // of correlations. The rationale is nodes that have more correlations
    // should be optimized first.
    SmallVector<std::pair<NodeOp, unsigned>> nodeAndNums;
    for (auto nodeAndList : corrAnal)
      nodeAndNums.push_back({nodeAndList.first, nodeAndList.second.size()});
    llvm::sort(nodeAndNums, [](auto a, auto b) { return a.second > b.second; });

    // Optimize the unroll factors from the most critical node.
    llvm::SmallDenseMap<NodeOp, FactorList> nodeUnrollFactorsMap;
    for (auto nodeAndNum : nodeAndNums) {
      auto node = nodeAndNum.first;
      auto corrList = corrAnal.getCorrelations(node);

      // If the correlation list is empty, which means the correlation analysis
      // failed, skip the current node.
      if (corrList.empty())
        continue;

      // Get the parallel factor and loop band associated with the current node.
      // Also initialize the unroll factors as one.
      auto parallelFactor = maxUnrollFactor.getValue();
      if (complexityAware && nodeParallelFactorMap.count(node))
        parallelFactor = nodeParallelFactorMap.lookup(node);
      auto band = getNodeLoopBand(node);
      auto factors = FactorList(band.size(), 1);

      // If the unroll factors already exist, which means the node is correlated
      // with a visited node, we overwrite the initialized unroll factors with
      // the existing one.
      if (nodeUnrollFactorsMap.count(node)) {
        assert(factors.size() == band.size() && "incorrect factor number");
        factors = nodeUnrollFactorsMap.lookup(node);
      }

      if (failed(getEvenlyDistributedFactors(parallelFactor, factors, band)))
        factors = getDistributedFactors(parallelFactor, band);

      LLVM_DEBUG(
          // clang-format off
          llvm::dbgs() << "\nCorrelations: " << nodeAndNum.second << "\n";
          llvm::dbgs() << "Parallel: " << parallelFactor << "\n";
          llvm::dbgs() << "Factors: { ";
          for (auto factor : factors)
            llvm::dbgs() << factor << " ";
          llvm::dbgs() << "}\n";
          llvm::dbgs() << "Node at " << node.getLoc() << ": \n" << node << "\n";
          // clang-format on
      );
      nodeUnrollFactorsMap[node] = factors;

      for (auto corr : corrList) {
        auto corrNode = corr.getCorrelatedNode(node);
        if (nodeUnrollFactorsMap.count(corrNode))
          continue;
        auto corrFactors = corr.permuteFactors(node, factors);

        LLVM_DEBUG(
            // clang-format off
            llvm::dbgs() << "----------\n";
            llvm::dbgs() << "Correlate Map: { ";
            for (auto factor : corr.getCorrelateMap(node))
              llvm::dbgs() << factor << " ";
            llvm::dbgs() << "}\n";
            llvm::dbgs() << "Correlated Factors: { ";
            for (auto factor : corrFactors)
              llvm::dbgs() << factor << " ";
            llvm::dbgs() << "}\n";
            llvm::dbgs() << "Correlated Node at " << corrNode.getLoc() << ": \n"
                         << corrNode << "\n";
            // clang-format on
        );
        nodeUnrollFactorsMap[corrNode] = corrFactors;
      }
    }

    // Apply unroll and jam to loops that is successfully calculated for
    // correlation-aware unroll factors.
    for (auto p : nodeUnrollFactorsMap) {
      auto band = getNodeLoopBand(p.first);
      if (hasEffectOnExternalBuffer(band.front()))
        applyLoopVectorization(band, p.second);
      else
        applyLoopUnrollJam(band, p.second);
    }

    // Apply naive unroll to other loops.
    for (auto p : nodeParallelFactorMap)
      if (!nodeUnrollFactorsMap.count(p.first))
        applyNaiveLoopUnroll(p.first, p.second);
  }

  void runOnOperation() override {
    auto func = getOperation();
    getNodeParallelFactorMap(func);
    if (correlationAware)
      applyCorrelationAwareUnroll(func);
    else
      for (auto p : nodeParallelFactorMap)
        applyNaiveLoopUnroll(p.first, p.second);
  }

private:
  llvm::SmallDenseMap<NodeOp, unsigned long> nodeParallelFactorMap;
};
} // namespace

std::unique_ptr<Pass> scalehls::createParallelizeDataflowNodePass(
    unsigned loopUnrollFactor, bool unrollPointLoopOnly, bool complexityAware,
    bool correlationAware) {
  return std::make_unique<ParallelizeDataflowNode>(
      loopUnrollFactor, unrollPointLoopOnly, complexityAware, correlationAware);
}
