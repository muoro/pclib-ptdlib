#include "predicates/analysis/CFGAnalyzer.h"
#include "predicates/analysis/InputValidator.h"
#include "predicates/export/CompletePathConditionBuilder.h"
#include "predicates/export/CompletePathConditionJSONExporter.h"
#include "predicates/export/JSONExporter.h"
#include "predicates/library/PCLibrary.h"
#include "predicates/library/PTDLibrary.h"
#include "predicates/solver/Z3PathConditionSolver.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace {
llvm::cl::OptionCategory ToolCategory("Predicates-TUWien options");
llvm::cl::opt<bool> BuildInfo(
    "build-info", llvm::cl::desc("Print build and solver configuration as JSON"),
    llvm::cl::init(false), llvm::cl::cat(ToolCategory));
llvm::cl::opt<bool> TraceAnalysis(
    "trace-analysis",
    llvm::cl::desc("Print every block state after processing it in each pass"),
    llvm::cl::init(false), llvm::cl::cat(ToolCategory));
llvm::cl::opt<bool> DumpCFG("dump-cfg",
                            llvm::cl::desc("Write and open Clang CFG graph"),
                            llvm::cl::init(false), llvm::cl::cat(ToolCategory));
llvm::cl::opt<bool>
    GraphOnly("graph-only",
              llvm::cl::desc("Show the CFG without running path analysis"),
              llvm::cl::init(false), llvm::cl::cat(ToolCategory));
llvm::cl::opt<unsigned>
    MaxPasses("max-passes",
              llvm::cl::desc("Maximum backward CFG propagation passes"),
              llvm::cl::init(CFGAnalyzer::DefaultMaxAnalysisPasses),
              llvm::cl::cat(ToolCategory));
llvm::cl::opt<std::string> ReportDirectory(
    "report-dir",
    llvm::cl::desc("Directory for analysis and test-data reports"),
    llvm::cl::init("reports"), llvm::cl::cat(ToolCategory));
llvm::cl::opt<bool> ExportOnly(
    "export-only",
    llvm::cl::desc("Export PClib without expanding or solving paths"),
    llvm::cl::init(false), llvm::cl::cat(ToolCategory));
llvm::cl::opt<std::string>
    PClibInput("pclib-input",
               llvm::cl::desc("Read an existing PClib instead of analyzing C"),
               llvm::cl::init(""), llvm::cl::cat(ToolCategory));
llvm::cl::list<std::string> SelectedPaths(
    "path",
    llvm::cl::desc("Target prefix or exit label (repeatable; empty is root)"),
    llvm::cl::ZeroOrMore, llvm::cl::cat(ToolCategory));
llvm::cl::opt<std::string>
    FunctionFilter("function",
                   llvm::cl::desc("Analyze only this named function"),
                   llvm::cl::init(""), llvm::cl::cat(ToolCategory));

bool analysisFailed = false;
size_t analyzedFunctions = 0;

std::optional<std::string> outputPath(const std::string &directory,
                                      const std::string &name) {
  llvm::SmallString<128> path(ReportDirectory);
  llvm::sys::path::append(path, directory);
  if (std::error_code error = llvm::sys::fs::create_directories(path)) {
    llvm::errs() << "Cannot create report directory: " << error.message()
                 << "\n";
    return std::nullopt;
  }
  llvm::sys::path::append(path, name);
  return std::string(path.str());
}

Z3PathConditionSolver::SolverOptions pipelineSolverOptions() {
  Z3PathConditionSolver::SolverOptions options;
  options.enforceInputSignature = true;
  options.progressInterval = 1000;
  return options;
}

bool processLibrary(const PCLibrary &library) {
  std::string error;
  if (!library.validate(error)) {
    llvm::errs() << "Invalid PClib: " << error << "\n";
    return false;
  }
  const auto pclibPath =
      outputPath("pclib", library.functionName + "_pclib.json");
  if (!pclibPath || !library.writeJSON(*pclibPath, error)) {
    llvm::errs() << "PClib export failed: " << error << "\n";
    return false;
  }
  size_t nodes = 0, predicates = 0;
  for (const auto &[label, path] : library.paths) {
    ++nodes;
    if (path.hasStoredEdgePredicate())
      ++predicates;
  }
  llvm::outs() << "PClib: " << *pclibPath << "\n"
               << "Structural nodes (including root): " << nodes << "\n"
               << "Stored incremental predicates: " << predicates << "\n"
               << "Complete exit objectives: "
               << library.paths.getExitPathLabels().size() << "\n";
  if (ExportOnly)
    return true;

  std::vector<std::string> targets;
  if (SelectedPaths.empty()) {
    targets = library.paths.getExitPathLabels();
  } else {
    targets.assign(SelectedPaths.begin(), SelectedPaths.end());
  }
  const auto conditions =
      CompletePathConditionBuilder::build(library.paths, targets, error);
  if (!conditions) {
    llvm::errs() << error << "\n";
    return false;
  }
  const auto conditionPath =
      outputPath("paths", library.functionName + "_path_conditions.json");
  if (!conditionPath ||
      !CompletePathConditionJSONExporter::exportToJSON(
          *conditions, library.functionName, library.configuredMaxPasses,
          library.analysisPasses, *conditionPath))
    return false;

  auto options = pipelineSolverOptions();
  options.wrapArithmetic = library.semantics == "signed-bv32-wrap";
  options.inputNames = library.inputNames;
  auto summary = Z3PathConditionSolver::solvePaths(*conditions, options);
  const auto solverPath =
      outputPath("solver", library.functionName + "_solver_results.json");
  const auto ptdPath =
      outputPath("ptdlib", library.functionName + "_ptdlib.json");
  if (!solverPath || !ptdPath ||
      !Z3PathConditionSolver::writeResults(*solverPath, summary))
    return false;
  if (!PTDLibrary::writeFromSolverResults(*ptdPath, library.libraryId(),
                                          library.functionName,
                                          library.inputNames, summary, error)) {
    llvm::errs() << "PTDlib export failed: " << error << "\n";
    return false;
  }
  llvm::outs() << "Targets solved: " << summary.results.size() << "\n"
               << "SAT: " << summary.statusCounts["sat"]
               << ", UNSAT: " << summary.statusCounts["unsat"]
               << ", unknown: " << summary.statusCounts["unknown"]
               << ", unsupported: " << summary.statusCounts["unsupported"]
               << "\nPTDlib: " << *ptdPath << "\n";
  return true;
}

class AnalysisASTConsumer : public ASTConsumer {
public:
  explicit AnalysisASTConsumer(ASTContext &context) : context_(context) {}

  bool HandleTopLevelDecl(DeclGroupRef declarations) override {
    for (Decl *declaration : declarations) {
      auto *function = dyn_cast<FunctionDecl>(declaration);
      if (!function || function->isImplicit() ||
          !function->doesThisDeclarationHaveABody())
        continue;
      const std::string name = function->getNameAsString();
      if (!FunctionFilter.empty() && name != FunctionFilter)
        continue;
      if (!seenFunctions_.insert(name).second) {
        llvm::errs() << "Duplicate or overloaded function name: " << name
                     << "\n";
        analysisFailed = true;
        continue;
      }
      ++analyzedFunctions;
      std::string error;
      if (!GraphOnly && !InputValidator::validate(*function, context_, error)) {
        llvm::errs() << "Unsupported function '" << name << "': " << error
                     << "\n";
        analysisFailed = true;
        continue;
      }
      auto cfg = CFG::buildCFG(function, function->getBody(), &context_,
                               CFG::BuildOptions());
      if (!cfg) {
        llvm::errs() << "Failed to build CFG for " << name << "\n";
        analysisFailed = true;
        continue;
      }
      if (DumpCFG || GraphOnly)
        cfg->viewCFG(context_.getLangOpts());
      if (GraphOnly)
        continue;

      CFGAnalyzer analyzer(cfg.get(), MaxPasses);
      if (TraceAnalysis)
        llvm::outs() << "\nFunction: " << name << "\n";
      analyzer.analyze(TraceAnalysis);
      const auto *entry = analyzer.getEntryNodes();
      if (!entry) {
        llvm::errs() << "Missing entry state for " << name << "\n";
        analysisFailed = true;
        continue;
      }
      PCLibrary library;
      library.functionName = name;
      const auto &sourceManager = context_.getSourceManager();
      bool invalidBuffer = false;
      const auto buffer = sourceManager.getBufferData(
          sourceManager.getMainFileID(), &invalidBuffer);
      if (invalidBuffer) {
        llvm::errs() << "Cannot read source text for " << name << "\n";
        analysisFailed = true;
        continue;
      }
      library.sourceSha256 = PCLibrary::sha256(buffer.str());
      for (const auto *parameter : function->parameters())
        library.inputNames.push_back(parameter->getNameAsString());
      library.configuredMaxPasses = MaxPasses;
      library.analysisPasses = analyzer.getAnalysisPasses();
      library.paths = *entry;
      const auto cfgPath = outputPath("cfg", name + "_cfg_analysis.json");
      if (!cfgPath || !JSONExporter::exportToJSON(analyzer, name, *cfgPath) ||
          !processLibrary(library))
        analysisFailed = true;
    }
    return true;
  }

private:
  ASTContext &context_;
  std::set<std::string> seenFunctions_;
};

class AnalysisFrontendAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &compiler,
                                                 StringRef) override {
    return std::make_unique<AnalysisASTConsumer>(compiler.getASTContext());
  }
};
} // namespace

int main(int argc, const char **argv) {
  auto parser = CommonOptionsParser::create(argc, argv, ToolCategory,
                                            llvm::cl::ZeroOrMore);
  if (!parser) {
    llvm::errs() << parser.takeError();
    return 1;
  }
  if (BuildInfo) {
    const auto defaults = pipelineSolverOptions();
    llvm::json::Object information{
        {"schema", "pccscc-build-info"},
        {"schema_version", 1},
        {"llvm", llvm::json::Object{{"version", LLVM_VERSION_STRING},
                                     {"revision", clang::getLLVMRevision()}}},
        {"clang", llvm::json::Object{{"version", clang::getClangFullVersion()},
                                      {"revision", clang::getClangRevision()}}},
        {"z3_available", Z3PathConditionSolver::isZ3Available()},
        {"semantics", "mathematical-integers"},
        {"default_max_passes", CFGAnalyzer::DefaultMaxAnalysisPasses},
        {"solver_defaults",
         llvm::json::Object{{"prefer_small_models", defaults.preferSmallModels},
                            {"small_model_bound", defaults.smallModelBound},
                            {"collect_timing", defaults.collectTiming},
                            {"pipeline_enforces_input_signature", defaults.enforceInputSignature},
                            {"progress_interval", static_cast<std::int64_t>(defaults.progressInterval)}}}};
    llvm::outs() << llvm::formatv("{0:2}\n",
                                  llvm::json::Value(std::move(information)));
    return 0;
  }
  if (MaxPasses == 0) {
    llvm::errs() << "--max-passes must be greater than zero\n";
    return 1;
  }
  if (ExportOnly && !SelectedPaths.empty()) {
    llvm::errs() << "--path selects solver objectives; omit --export-only\n";
    return 1;
  }
  if (!PClibInput.empty()) {
    if (!parser->getSourcePathList().empty() || GraphOnly || DumpCFG ||
        !FunctionFilter.empty() || MaxPasses.getNumOccurrences()) {
      llvm::errs() << "--pclib-input uses the stored function and pass bound; "
                      "do not combine with source/CFG/function/bound options\n";
      return 1;
    }
    std::string error;
    auto library = PCLibrary::readJSON(PClibInput, error);
    if (!library) {
      llvm::errs() << "Cannot read PClib: " << error << "\n";
      return 1;
    }
    return processLibrary(*library) ? 0 : 1;
  }
  if (parser->getSourcePathList().empty()) {
    llvm::errs() << "Supply a C source file or --pclib-input\n";
    return 1;
  }
  // File names form report identities. Multiple translation units may contain
  // equal function names, so analyze them in separate invocations.
  if (parser->getSourcePathList().size() != 1) {
    llvm::errs() << "Analyze one translation unit per invocation\n";
    return 1;
  }
  ClangTool tool(parser->getCompilations(), parser->getSourcePathList());
  const int result =
      tool.run(newFrontendActionFactory<AnalysisFrontendAction>().get());
  if (analyzedFunctions == 0 && result == 0) {
    llvm::errs() << "No matching function definitions found\n";
    return 1;
  }
  return result != 0 ? result : (analysisFailed ? 1 : 0);
}
