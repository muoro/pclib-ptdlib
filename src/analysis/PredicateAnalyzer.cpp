#include "predicates/analysis/PredicateAnalyzer.h"
#include "predicates/utils/ConditionExtractor.h"
#include "predicates/utils/VariableSubstitution.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace PredicateUtils;

void PredicateAnalyzer::propagateFromSuccessor(
    CFGBlock *block, const CFGBlock *successor,
    const PredicateAnalyzer &successorState, unsigned successorIndex) {
  if (const Stmt *terminator = block->getTerminator().getStmt()) {
    switch (terminator->getStmtClass()) {
    case Stmt::IfStmtClass:
    case Stmt::WhileStmtClass:
    case Stmt::ForStmtClass:
      propagateThroughBranch(block, successorIndex, successorState);
      break;
    case Stmt::BinaryOperatorClass:
      // Short-circuit operands are decisions in their own CFG blocks, even
      // though their terminator is not an IfStmt/WhileStmt/ForStmt.
      if (cast<BinaryOperator>(terminator)->isLogicalOp()) {
        propagateThroughBranch(block, successorIndex, successorState);
      } else {
        llvm::errs() << "Unhandled binary operator terminator.\n";
      }
      break;
    case Stmt::BreakStmtClass:
    case Stmt::ContinueStmtClass:
      // These are CFG terminators but introduce no branch decision. Their
      // single successor already encodes the loop exit or continuation target.
      propagateThroughUnconditionalEdge(block, successor, successorState);
      break;
    default:
      llvm::errs() << "Unhandled terminator type.\n";
      break;
    }
  } else {
    propagateThroughUnconditionalEdge(block, successor, successorState);
  }
}

void PredicateAnalyzer::propagateThroughUnconditionalEdge(
    CFGBlock *block, const CFGBlock *successor,
    const PredicateAnalyzer &successorState) {
  (void)successor;
  // A self-successor needs a snapshot so this transfer never visits nodes that
  // it has just inserted. Other CFG edges can read the successor state directly.
  std::optional<PathNodeSet> snapshot;
  if (&successorState == this) {
    snapshot = successorState.nodes_;
  }
  const PathNodeSet &incoming =
      snapshot ? *snapshot : successorState.nodes_;
  for (const auto &[label, path] : incoming) {
    auto &target = nodes_.getOrCreateNode(label);
    const bool alreadyReachesExit = target.reachesExit;
    target = path;
    target.reachesExit = target.reachesExit || alreadyReachesExit;
    VariableSubstitution::substituteVariablesInBlock(block, target);
  }
}

void PredicateAnalyzer::propagateThroughBranch(
    CFGBlock *block, unsigned successorIndex,
    const PredicateAnalyzer &successorState) {
  const BranchType branchType = successorIndex == 0 ? BranchType::TrueBranch
                                                   : BranchType::FalseBranch;
  const std::string prefix =
      ConditionExtractor::getBranchPrefix(block, branchType);
  const std::string trueLabel = PathLabels::trueSiblingLabel(prefix);

  // Materialize the true guard even when Clang prunes its outgoing edge. False
  // records must still have a canonical same-context predicate to negate.
  if (!nodes_.containsNode(trueLabel)) {
    auto &guard = nodes_.getOrCreateNode(trueLabel);
    guard.edgePredicate = ConditionExtractor::getTerminatorCondition(
        block, BranchType::TrueBranch);
    // An omitted for-loop guard is the constant true condition.
    const auto *forStmt = dyn_cast_or_null<ForStmt>(block->getTerminatorStmt());
    if (!guard.hasStoredEdgePredicate() && forStmt && !forStmt->getCond()) {
      guard.edgePredicate = "true";
    }
    VariableSubstitution::substituteVariablesInBlock(block, guard);
  }

  std::optional<PathNodeSet> snapshot;
  if (&successorState == this) {
    snapshot = successorState.nodes_;
  }
  const PathNodeSet &incoming =
      snapshot ? *snapshot : successorState.nodes_;
  for (const auto &[label, path] : incoming) {
    const std::string newLabel =
        PathLabels::prepend(prefix, label);
    const bool isNew = !nodes_.containsNode(newLabel);
    auto &target = nodes_.getOrCreateNode(newLabel);
    target.reachesExit = target.reachesExit || path.reachesExit;
    if (label.empty() || !isNew) {
      continue;
    }
    // Only extend the structural label. The ancestor guard is stored in its
    // own record and is NOT inserted as a conjunct into this edge predicate.
    target.edgePredicate = path.edgePredicate;
    VariableSubstitution::substituteVariablesInBlock(block, target);
  }
}

void PredicateAnalyzer::printInfos() const {
  size_t nodes = 0, predicates = 0, exitPaths = 0;
  for (const auto &[label, path] : nodes_) {
    (void)label;
    ++nodes;
    predicates += path.hasStoredEdgePredicate();
    exitPaths += path.reachesExit;
  }
  llvm::outs() << "  Nodes: " << nodes
               << " | stored predicates: " << predicates
               << " | exit-reaching paths: " << exitPaths << "\n";
  for (const auto &[label, path] : nodes_) {
    llvm::outs() << "  " << (label.empty() ? "<root>" : label)
                 << " | reaches_exit: " << (path.reachesExit ? "true" : "false");
    if (!label.empty()) {
      if (PathLabels::isFalseEdge(label)) {
        const std::string sibling = PathLabels::trueSiblingLabel(label);
        llvm::outs() << " | negated_from: " << sibling;
      } else {
        llvm::outs() << " | predicate: " << path.edgePredicate;
      }
    }
    llvm::outs() << "\n";
  }
}
