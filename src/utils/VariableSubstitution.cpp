#include "predicates/utils/VariableSubstitution.h"
#include "predicates/model/PathNode.h"
#include "predicates/utils/ExpressionUtils.h"

#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"

#include <vector>

using namespace clang;
using namespace PredicateUtils;

void VariableSubstitution::substituteVariablesInBlock(
    CFGBlock *block, PathNode &node) {
  if (!node.hasStoredEdgePredicate()) {
    return;
  }
  // Apply assignments and initializers once, in reverse execution order. Even
  // an identifier guard requires substitution; predicates contain no labels.
  for (auto it = block->rbegin(); it != block->rend(); ++it) {
    if (const auto statement = it->getAs<CFGStmt>()) {
      substituteStatement(statement->getStmt(), node.edgePredicate);
    }
  }
}

void VariableSubstitution::substituteStatement(const Stmt *statement,
                                               std::string &predicate) {
  if (const auto *declarations = dyn_cast<DeclStmt>(statement)) {
    // Clang normally splits declaration groups into individual CFG statements.
    // Handle an unsplit group too: int x = a, y = x + 1 must replace y before x.
    std::vector<const VarDecl *> variables;
    for (const auto *declaration : declarations->decls()) {
      if (const auto *variable = dyn_cast<VarDecl>(declaration)) {
        if (variable->hasInit()) {
          variables.push_back(variable);
        }
      }
    }
    for (auto it = variables.rbegin(); it != variables.rend(); ++it) {
      ExpressionUtils::substituteVariable(
          predicate, (*it)->getNameAsString(),
          "(" + ExpressionUtils::exprToString((*it)->getInit()) + ")");
    }
    return;
  }

  std::string leftSide;
  std::string rightSide;
  if (ExpressionUtils::extractAssignmentOperands(statement, leftSide,
                                                 rightSide)) {
    ExpressionUtils::substituteVariable(predicate, leftSide,
                                        "(" + rightSide + ")");
  }
}
