#ifndef PREDICATES_MODEL_SOLVER_RESULT_H
#define PREDICATES_MODEL_SOLVER_RESULT_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Backend-independent query results shared by solvers and persistent libraries.
struct SolverResult {
  std::string pathLabel;
  std::string predicate;
  std::string status;
  std::map<std::string, std::string> model;
  double solveTimeMs = 0.0;
  std::string error;
  // Checked against the translated formula with all model symbols fixed.
  // This is not evidence of execution of the source program.
  bool modelValidated = false;
};

struct SolverRunSummary {
  std::string inputModel = "mathematical-integers";
  std::vector<SolverResult> results;
  double totalSolveTimeMs = 0.0;
  std::map<std::string, size_t> statusCounts;
};

#endif // PREDICATES_MODEL_SOLVER_RESULT_H
