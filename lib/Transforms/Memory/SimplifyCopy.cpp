//===----------------------------------------------------------------------===//
//
// Copyright 2020-2021 The ScaleHLS Authors.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "scalehls/Transforms/Passes.h"
#include "scalehls/Transforms/Utils.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "scalehls-simplify-copy"

using namespace mlir;
using namespace scalehls;
using namespace hls;

static Value findBuffer(Value memref) {
  if (memref.isa<BlockArgument>())
    return memref;
  else if (auto buffer = memref.getDefiningOp<BufferOp>())
    return buffer.getMemref();
  else if (auto viewOp = memref.getDefiningOp<ViewLikeOpInterface>())
    return findBuffer(viewOp.getViewSource());
  return Value();
}

static void findBufferUsers(Value memref, SmallVector<Operation *> &users) {
  for (auto user : memref.getUsers()) {
    if (auto viewOp = dyn_cast<ViewLikeOpInterface>(user))
      findBufferUsers(viewOp->getResult(0), users);
    else
      users.push_back(user);
  }
}

static bool crossRegionDominates(Operation *a, Operation *b) {
  if (a == b)
    return true;
  if (b->isAncestor(a))
    return false;
  while (a->getParentOp() && !a->getParentOp()->isAncestor(b))
    a = a->getParentOp();
  assert(a->getParentOp() && "reach top-level module op");
  return DominanceInfo().dominates(a, b);
}

namespace {
struct SimplifyBufferCopy : public OpRewritePattern<memref::CopyOp> {
  using OpRewritePattern<memref::CopyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::CopyOp copy,
                                PatternRewriter &rewriter) const override {
    LLVM_DEBUG(llvm::dbgs() << "\nCurrent copy: " << copy << "\n";);

    // If the source and target buffers are allocated in different memory space,
    // return failure.
    auto sourceType = copy.getSource().getType().template cast<MemRefType>();
    auto targetType = copy.getTarget().getType().template cast<MemRefType>();
    if (sourceType.getMemorySpaceAsInt() != targetType.getMemorySpaceAsInt())
      return failure();

    LLVM_DEBUG(llvm::dbgs() << "Located at the same memory space\n";);

    // Both the source and target buffers should be block arguments or defined
    // by BufferOp, otherwise return failure.
    auto source = findBuffer(copy.getSource());
    auto target = findBuffer(copy.getTarget());
    if (!source || !target)
      return failure();

    LLVM_DEBUG(llvm::dbgs() << "Defined by block argument or BufferOp\n";);

    // If both the source and target buffers are block arguments, return failure
    // as either of them can be eliminated.
    auto sourceBuf = source.getDefiningOp<BufferOp>();
    auto targetBuf = target.getDefiningOp<BufferOp>();
    if (!sourceBuf && !targetBuf)
      return failure();

    LLVM_DEBUG(llvm::dbgs() << "At least one buffer is replaceable\n";);

    // We adopt a conservative condition that all users of the source buffer
    // must dominate the copy and all users of the target buffer must be
    // dominated by the copy.
    SmallVector<Operation *> sourceUsers;
    SmallVector<Operation *> targetUsers;
    findBufferUsers(source, sourceUsers);
    findBufferUsers(target, targetUsers);

    if (!llvm::all_of(sourceUsers, [&](Operation *user) {
          return crossRegionDominates(user, copy);
        }))
      return failure();
    if (!llvm::all_of(targetUsers, [&](Operation *user) {
          return crossRegionDominates(copy, user);
        }))
      return failure();

    LLVM_DEBUG(llvm::dbgs() << "Dominances are valid\n");

    auto sourceView = copy.getSource().getDefiningOp();
    auto targetView = copy.getTarget().getDefiningOp();
    DominanceInfo domInfo;

    // To replace the target buffer, the buffer must be directly defined by a
    // BufferOp without view. Meanwhile, the source view should either be a
    // block argument or dominate all users of the target buffer.
    // TODO: The second condition is quite conservative and could be improved by
    // analyzing whether the source view can be replaced to the location of the
    // target buffer.
    if (targetBuf && targetBuf == targetView &&
        (!sourceView || llvm::all_of(targetUsers, [&](Operation *user) {
          return domInfo.dominates(sourceView, user);
        }))) {
      LLVM_DEBUG(llvm::dbgs() << "Target and copy is erased\n");

      rewriter.replaceOp(targetBuf, copy.getSource());
      rewriter.eraseOp(copy);
      return success();
    }

    // Similarly, we need the same conditions to replace the source buffer.
    if (sourceBuf && sourceBuf == sourceView &&
        (!targetView || llvm::all_of(sourceUsers, [&](Operation *user) {
          return domInfo.dominates(targetView, user);
        }))) {
      // If the source buffer has initial value, the value must be pertained by
      // the target buffer after the replacement. Therefore, we have some
      // additional conditions here to check.
      if (sourceBuf.getInitValue()) {
        if (!targetBuf || (targetBuf.getInitValue() && targetBuf != targetView))
          return failure();
        targetBuf.setInitValueAttr(sourceBuf.getInitValue().value());
      }
      LLVM_DEBUG(llvm::dbgs() << "Source and copy are erased\n");

      rewriter.replaceOp(sourceBuf, copy.getTarget());
      rewriter.eraseOp(copy);
      return success();
    }
    return failure();
  }
};
} // namespace

namespace {
struct SimplifyCopy : public SimplifyCopyBase<SimplifyCopy> {
  void runOnOperation() override {
    auto func = getOperation();
    auto context = func.getContext();

    mlir::RewritePatternSet patterns(context);
    patterns.add<SimplifyBufferCopy>(context);
    (void)applyPatternsAndFoldGreedily(func, std::move(patterns));
  }
};
} // namespace

std::unique_ptr<Pass> scalehls::createSimplifyCopyPass() {
  return std::make_unique<SimplifyCopy>();
}
