#include "predicates/utils/ConditionExtractor.h"
#include "predicates/utils/ExpressionUtils.h"

#include "clang/AST/Stmt.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/Support/Casting.h"

#include <string>

using namespace clang;
using namespace PredicateUtils;

std::string
ConditionExtractor::getTerminatorCondition(const CFGBlock *block,
                                           BranchType branchType) {
  if (!block || block->succ_size() != 2) {
    return {};
  }

  // Recognize only the decision kinds supported by the analysis. The source
  // condition is an atomic-only fallback for blocks with no last CFGStmt.
  const Expr *sourceCondition = nullptr;
  const Stmt *term = block->getTerminatorStmt();
  if (const auto *ifStmt = dyn_cast_or_null<IfStmt>(term)) {
    sourceCondition = ifStmt->getCond();
  } else if (const auto *whileStmt = dyn_cast_or_null<WhileStmt>(term)) {
    sourceCondition = whileStmt->getCond();
  } else if (const auto *forStmt = dyn_cast_or_null<ForStmt>(term)) {
    sourceCondition = forStmt->getCond();
  } else if (const auto *binary = dyn_cast_or_null<BinaryOperator>(term)) {
    if (!binary->isLogicalOp()) {
      return {};
    }
    sourceCondition = binary->getLHS();
  } else {
    return {};
  }

  // For if (A || B), the IfStmt at B still contains A || B. Read B's
  // evaluated expression instead; CFG edges already encode short-circuiting.
  const Expr *condExpr = block->getLastCondition();
  if (!condExpr) {
    condExpr = sourceCondition;
  }
  if (!condExpr) {
    return {};
  }
  const Expr *guard = condExpr->IgnoreParenImpCasts();
  const auto *binary = dyn_cast<BinaryOperator>(guard);
  if (binary && binary->isLogicalOp()) {
    // Do not silently fall back to a compound source condition or to true.
    return {};
  }

  std::string condition = "(" + ExpressionUtils::exprToString(condExpr) + ")";
  const bool booleanExpression =
      (binary && binary->isComparisonOp()) || isa<CXXBoolLiteralExpr>(guard);
  if (!booleanExpression) {
    // C scalar guards select true exactly when their value is nonzero.
    condition = "(" + condition + " != 0)";
  }

  // Polarity comes from the CFG successor slot, never the target pointer.
  if (branchType == BranchType::FalseBranch) {
    condition = "not" + condition;
  }
  return condition;
}

std::string
ConditionExtractor::getBranchPrefix(const clang::CFGBlock *block,
                                    BranchType branchType) {
  std::string prefix = "B" + std::to_string(block->getBlockID());

  prefix += branchTypeToString(branchType);
  return prefix;
}

std::string ConditionExtractor::branchTypeToString(BranchType branchType) {
  switch (branchType) {
  case BranchType::TrueBranch:
    return "T";
  case BranchType::FalseBranch:
    return "F";
  }
  return "";
}
