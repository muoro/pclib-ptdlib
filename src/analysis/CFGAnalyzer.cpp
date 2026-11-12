#include "predicates/analysis/CFGAnalyzer.h"

#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace clang;

CFGAnalyzer::CFGAnalyzer(clang::CFG *cfg, unsigned maxAnalysisPasses)
    : maxAnalysisPasses_(maxAnalysisPasses), cfg_(cfg) {}

void CFGAnalyzer::printCFGNodesOrder() {
  llvm::outs() << "CFG processing order:";
  for (const auto *block : cfg_->const_nodes()) {
    llvm::outs() << " B" << block->getBlockID();
  }
  llvm::outs() << "\n";
}

void CFGAnalyzer::analyze(bool verbose) {
  if (verbose) {
    printCFGNodesOrder();
  }
  unsigned count = 1;
  analysisPasses_ = 0;
  blockPredicateAnalyzers_.clear();
  for (auto *block : cfg_->const_nodes()) {
    blockPredicateAnalyzers_[block];
  }
  // Only the actual CFG exit seeds a completed empty path. Propagation carries
  // this marker through assignments and adds labels at branch decisions.
  blockPredicateAnalyzers_[&cfg_->getExit()]
      .getNodes()
      .getOrCreateNode("")
      .reachesExit = true;
  std::set<unsigned> loopHeaderBlockIds;

  for (auto *block : cfg_->const_nodes()) {
    if (isLoopTerminator(block)) {
      loopHeaderBlockIds.insert(block->getBlockID());
    }
  }

  do {
    analysisPasses_ = count;
    if (verbose) {
      llvm::outs() << "\n=== Backward analysis: pass " << count << "/"
                   << maxAnalysisPasses_ << " ===\n";
    }
    for (auto *block : cfg_->const_nodes()) {
      processBlock(block);
      if (verbose) {
        printBlock(block);
      }
    }
    ++count;
    if (loopHeaderBlockIds.empty()) {
      break;
    }
  } while (count <= maxAnalysisPasses_);
}

bool CFGAnalyzer::isLoopTerminator(clang::CFGBlock *block) {
  if (block == nullptr) {
    return false;
  }

  const Stmt *terminator = block->getTerminatorStmt();
  return isa_and_nonnull<WhileStmt>(terminator) ||
         isa_and_nonnull<ForStmt>(terminator);
}

void CFGAnalyzer::processBlock(clang::CFGBlock *block) {
  if (block == nullptr) {
    llvm::errs() << "Block is null\n";
    return;
  }

  PredicateAnalyzer &predicateAnalyzer = blockPredicateAnalyzers_[block];

  unsigned successorIndex = 0;
  for (const auto &successor : block->succs()) {
    // Count null/pruned slots too: slot 0 is T and slot 1 is F for supported
    // binary decisions, even if both edges target the same successor block.
    const unsigned edgeIndex = successorIndex++;
    clang::CFGBlock *successorBlock = successor;
    if (successorBlock == nullptr) {
      llvm::errs() << "Warning: Encountered null successor block\n , Block ID: "
                   << block->getBlockID() << "\n";
      continue;
    }

    auto it = blockPredicateAnalyzers_.find(successorBlock);
    if (it != blockPredicateAnalyzers_.end()) {
      predicateAnalyzer.propagateFromSuccessor(block, successorBlock, it->second,
                                        edgeIndex);
    }
  }
}

void CFGAnalyzer::printBlock(const clang::CFGBlock *block) const {
  if (!block) {
    return;
  }
  auto it = blockPredicateAnalyzers_.find(const_cast<clang::CFGBlock *>(block));
  if (it == blockPredicateAnalyzers_.end()) {
    return;
  }
  llvm::outs() << "\nBlock B" << block->getBlockID() << " | successors:";
  if (block->succ_empty()) {
    llvm::outs() << " none";
  }
  for (const auto &successor : block->succs()) {
    if (const auto *next = successor.getReachableBlock()) {
      llvm::outs() << " B" << next->getBlockID();
    } else {
      llvm::outs() << " <pruned>";
    }
  }
  llvm::outs() << "\n";
  if (block == &cfg_->getEntry()) {
    llvm::outs() << "  Context: function entry (original inputs)\n";
  } else if (block == &cfg_->getExit()) {
    llvm::outs() << "  Context: CFG exit\n";
  } else {
    llvm::outs() << "  Context: block-entry values\n";
  }
  it->second.printInfos();
}

void CFGAnalyzer::printBlocks() const {
  llvm::outs() << "\n=== CFG analysis state after " << analysisPasses_
               << " propagation pass(es) ===\n";
  for (const auto *block : cfg_->const_nodes()) {
    printBlock(block);
  }
}

const PathNodeSet *CFGAnalyzer::getEntryNodes() const {
  CFGBlock *entryBlock = &(cfg_->getEntry());
  auto it = blockPredicateAnalyzers_.find(entryBlock);
  if (it == blockPredicateAnalyzers_.end()) {
    return nullptr;
  }
  return &it->second.getNodes();
}
