#ifndef Z3_PATH_CONDITION_SOLVER_H
#define Z3_PATH_CONDITION_SOLVER_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "predicates/model/CompletePathCondition.h"
#include "predicates/model/SolverResult.h"

class Z3PathConditionSolver {
public:
  struct SolverOptions {
    bool wrapArithmetic = false; // Legacy PClib v1/v2 only.
    bool preferSmallModels;
    int smallModelBound;
    bool collectTiming;
    size_t progressInterval;
    // PClib inputs are signed 32-bit scalars. An empty enforced signature is
    // meaningful for functions without parameters.
    std::vector<std::string> inputNames;
    bool enforceInputSignature = false;

    SolverOptions()
        : preferSmallModels(true), smallModelBound(100), collectTiming(true),
          progressInterval(0) {}
  };

  // Preserve the original solver-qualified names for existing callers.
  using SolverResult = ::SolverResult;
  using SolverRunSummary = ::SolverRunSummary;

  static bool isZ3Available();
  static SolverRunSummary
  solvePaths(const std::vector<CompletePathCondition> &conditions,
             const SolverOptions &options = {});
  static std::optional<SolverRunSummary>
  solveFromJSON(const std::string &inputFilename,
                const SolverOptions &options = {});
  static bool writeResults(const std::string &outputFilename,
                           const SolverRunSummary &summary);
};

#endif // Z3_PATH_CONDITION_SOLVER_H
