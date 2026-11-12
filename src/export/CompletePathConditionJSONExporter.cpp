#include "predicates/export/CompletePathConditionJSONExporter.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <system_error>

bool CompletePathConditionJSONExporter::exportToJSON(
    const std::vector<CompletePathCondition> &paths,
    const std::string &functionName, unsigned configuredMaxPasses,
    unsigned analysisPasses,
    const std::string &outputFilename) {
  llvm::json::Array pathArray;
  for (const CompletePathCondition &path : paths) {
    pathArray.push_back(llvm::json::Object{{"path_label", path.pathLabel},
                                          {"predicate", path.predicate}});
  }

  llvm::json::Object root{
      {"function", functionName},
      {"configured_max_passes",
       static_cast<std::int64_t>(configuredMaxPasses)},
      {"analysis_passes", static_cast<std::int64_t>(analysisPasses)},
      {"total_paths", static_cast<std::int64_t>(paths.size())},
      {"path_conditions", std::move(pathArray)},
  };

  std::error_code error;
  llvm::raw_fd_ostream output(outputFilename, error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << "Failed to open complete-path output file '"
                 << outputFilename << "': " << error.message() << "\n";
    return false;
  }

  output << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(root)));
  output.flush();
  if (output.has_error()) {
    llvm::errs() << "Failed to write complete-path output file '"
                 << outputFilename << "'\n";
    return false;
  }
  return true;
}
