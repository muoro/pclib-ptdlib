#ifndef JSON_EXPORTER_H
#define JSON_EXPORTER_H

#include "predicates/analysis/CFGAnalyzer.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/JSON.h"
#include <string>

class JSONExporter {
public:
  // Static method to export CFG analysis to JSON
  static bool exportToJSON(const CFGAnalyzer &analyzer,
                           const std::string &functionName,
                           const std::string &filename);

private:
  // Helper methods for JSON construction
  static std::string getTerminatorType(const clang::CFGBlock *block);
  static llvm::json::Object
  createBlockJSON(const clang::CFGBlock *block,
                  const PredicateAnalyzer &analyzer);
};

#endif
