#ifndef CONDITION_EXTRACTOR_H
#define CONDITION_EXTRACTOR_H

#include "clang/Analysis/CFG.h"
#include <string>

namespace PredicateUtils {

enum class BranchType { TrueBranch, FalseBranch };

class ConditionExtractor {
public:
  // Extract the block-local guard, including short-circuit operands.
  // An unavailable/unsupported guard is reported as an empty string.
  static std::string getTerminatorCondition(const clang::CFGBlock *block,
                                            BranchType branchType);

  // Explicit polarity preserves distinct edges with the same successor.
  // Get branch prefix (e.g., "B1T", "B2F").
  static std::string getBranchPrefix(const clang::CFGBlock *block,
                                     BranchType branchType);

private:
  static std::string branchTypeToString(BranchType branchType);
};

} // namespace PredicateUtils

#endif
