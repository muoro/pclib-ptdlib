#ifndef CFG_ANALYZER_H
#define CFG_ANALYZER_H

#include "predicates/analysis/PredicateAnalyzer.h"

#include "clang/Analysis/CFG.h"

#include "llvm/ADT/MapVector.h"

#include <string>

class CFGAnalyzer {
public:
  static constexpr unsigned DefaultMaxAnalysisPasses = 3;

  CFGAnalyzer(clang::CFG *cfg,
              unsigned maxAnalysisPasses = DefaultMaxAnalysisPasses);
  const PathNodeSet *getEntryNodes() const;
  unsigned getEntryBlockId() const { return cfg_->getEntry().getBlockID(); }
  unsigned getAnalysisPasses() const { return analysisPasses_; }

  void analyze(bool verbose = true);
  void printBlocks() const;
  void printBlock(const clang::CFGBlock *block) const;

  // Getter for block analyzers (for JSON export)
  const llvm::MapVector<clang::CFGBlock *, PredicateAnalyzer> &
  getBlockAnalyzers() const {
    return blockPredicateAnalyzers_;
  }

private:
  unsigned maxAnalysisPasses_ = DefaultMaxAnalysisPasses;
  unsigned analysisPasses_ = 0;
  clang::CFG *cfg_;

  llvm::MapVector<clang::CFGBlock *, PredicateAnalyzer>
      blockPredicateAnalyzers_;

  static bool isLoopTerminator(clang::CFGBlock *block);
  void processBlock(clang::CFGBlock *block);
  void printCFGNodesOrder();
};

#endif
