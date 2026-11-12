#include "predicates/export/JSONExporter.h"
#include "predicates/model/PathLabel.h"

#include "clang/AST/Stmt.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"


#include <string>

bool JSONExporter::exportToJSON(const CFGAnalyzer &analyzer,
                                const std::string &functionName,
                                const std::string &filename) {
  // Get block analyzers from CFGAnalyzer
  const auto &blockAnalyzers = analyzer.getBlockAnalyzers();

  // Build JSON blocks array
  llvm::json::Array blocks;
  for (const auto &entry : blockAnalyzers) {
    blocks.push_back(createBlockJSON(entry.first, entry.second));
  }

  // Build root JSON object
  llvm::json::Object cfgAnalysis{
      {"representation", "incremental-edge-constraints"},
      {"false_edges", "negated_from"},
      {"total_blocks", static_cast<int64_t>(blockAnalyzers.size())},
      {"entry_block_id", static_cast<int64_t>(analyzer.getEntryBlockId())},
      {"blocks", std::move(blocks)}};

  llvm::json::Object root{{"cfg_analysis_results", std::move(cfgAnalysis)}};

  // Write to file
  std::error_code EC;
  llvm::raw_fd_ostream jsonFile(filename, EC);
  if (EC) {
    llvm::errs() << "Error: Could not open file " << filename
                 << " for writing: " << EC.message() << "\n";
    return false;
  }

  jsonFile << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
  jsonFile.close();

  llvm::outs() << "CFG analysis results for function '" << functionName
               << "' exported to: " << filename << "\n";
  return true;
}

std::string JSONExporter::getTerminatorType(const clang::CFGBlock *block) {
  if (const clang::Stmt *terminator = block->getTerminatorStmt()) {
    if (llvm::isa<clang::IfStmt>(terminator)) {
      return "if";
    }
    if (llvm::isa<clang::WhileStmt>(terminator)) {
      return "while";
    }
    if (llvm::isa<clang::ForStmt>(terminator)) {
      return "for";
    }
    if (llvm::isa<clang::SwitchStmt>(terminator)) {
      return "switch";
    }
    return "other";
  }
  return "none";
}

llvm::json::Object
JSONExporter::createBlockJSON(const clang::CFGBlock *block,
                              const PredicateAnalyzer &analyzer) {
  // Get terminator information
  const std::string terminatorType = getTerminatorType(block);

  // Get successors
  llvm::json::Array successors;
  for (const auto &successor : block->succs()) {
    const clang::CFGBlock *successorBlock = successor;
    if (successorBlock != nullptr) {
      successors.push_back(static_cast<int64_t>(successorBlock->getBlockID()));
    }
  }

  // Export every structural node, including false labels and the root,
  // which have no explicitly stored edge predicate.
  llvm::json::Array conditions;
  for (const auto &[label, path] : analyzer.getNodes()) {
    llvm::json::Object conditionObj{{"path_label", label}};
    if (!label.empty()) {
      if (PathLabels::isFalseEdge(label)) {
        conditionObj["negated_from"] = PathLabels::trueSiblingLabel(label);
      } else {
        conditionObj["predicate"] = path.edgePredicate;
      }
    }
    conditions.push_back(std::move(conditionObj));
  }

  // Build block JSON object
  return llvm::json::Object{
      {"block_id", static_cast<int64_t>(block->getBlockID())},
      {"block_name", "B" + std::to_string(block->getBlockID())},
      {"terminator_type", terminatorType},
      {"successors", std::move(successors)},
      {"conditions", std::move(conditions)}};
}
