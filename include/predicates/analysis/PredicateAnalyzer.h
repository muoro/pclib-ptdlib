#ifndef PREDICATE_ANALYZER_H
#define PREDICATE_ANALYZER_H

#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include "predicates/model/PathNode.h"

class PredicateAnalyzer {
public:
  PredicateAnalyzer() = default;

  void propagateFromSuccessor(clang::CFGBlock *block, const clang::CFGBlock *successor,
                       const PredicateAnalyzer &successorState,
                       unsigned successorIndex);
  void printInfos() const;

  PathNodeSet &getNodes() { return nodes_; }
  const PathNodeSet &getNodes() const { return nodes_; }

private:
  PathNodeSet nodes_;

  void propagateThroughBranch(clang::CFGBlock *block,
                               unsigned successorIndex,
                               const PredicateAnalyzer &successorState);
  void propagateThroughUnconditionalEdge(clang::CFGBlock *block,
                            const clang::CFGBlock *successor,
                            const PredicateAnalyzer &successorState);
};

#endif
