#ifndef VARIABLE_SUBSTITUTION_H
#define VARIABLE_SUBSTITUTION_H

#include "clang/Analysis/CFG.h"
#include <string>

struct PathNode;

namespace PredicateUtils {

class VariableSubstitution {
public:
  // Update an existing node; substitution never creates structural nodes.
  static void substituteVariablesInBlock(clang::CFGBlock *block, PathNode &node);

private:
  // Apply an assignment or declaration initializer in backward order.
  static void substituteStatement(const clang::Stmt *statement,
                                  std::string &predicate);
};

} // namespace PredicateUtils

#endif
