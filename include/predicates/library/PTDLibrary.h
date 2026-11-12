#ifndef PTD_LIBRARY_H
#define PTD_LIBRARY_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "predicates/model/SolverResult.h"

// Concrete input vectors associated with selected PClib objectives. Multiple
// tests may target the same objective; unsolved objectives have no test record.
class PTDLibrary {
public:
  struct TestInput {
    std::string id;
    std::string pathLabel;
    std::map<std::string, std::int32_t> inputs;
  };

  std::string inputModel = "mathematical-integers";
  std::string pclibId;
  std::string functionName;
  std::vector<std::string> inputNames;
  std::vector<TestInput> tests;
  std::vector<SolverResult> diagnostics;

  static std::optional<PTDLibrary>
  fromSolverResults(const std::string &pclibId, const std::string &functionName,
                    const std::vector<std::string> &inputNames,
                    const SolverRunSummary &summary,
                    std::string &error);
  static bool
  writeFromSolverResults(const std::string &filename,
                         const std::string &pclibId,
                         const std::string &functionName,
                         const std::vector<std::string> &inputNames,
                         const SolverRunSummary &summary,
                         std::string &error);

  // Writes grouped schema v2 in a stable, human-readable section order.
  bool writeJSON(const std::string &filename, std::string &error) const;
  // Accepts grouped v2 and legacy flat v1. Checks schema, summaries, input
  // vectors and linkage inside the document. Reading does not verify constraints
  // against PClib or execute the analyzed C function.
  static std::optional<PTDLibrary> readJSON(const std::string &filename,
                                            std::string &error);
};

#endif
