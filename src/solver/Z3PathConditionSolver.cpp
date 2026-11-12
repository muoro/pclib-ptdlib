#include "predicates/solver/Z3PathConditionSolver.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <set>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef HAVE_Z3
#include "PredicateParser.h"

#endif

namespace {

std::string formatSeconds(double seconds) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << seconds << "s";
  return stream.str();
}

std::string makeProgressBar(size_t done, size_t total, size_t width = 30) {
  if (total == 0) {
    return std::string(width, '-');
  }

  const size_t filled = std::min(width, (done * width) / total);
  return std::string(filled, '#') + std::string(width - filled, '-');
}

void printSolverProgress(size_t done, size_t total,
                         std::chrono::steady_clock::time_point start) {
  const auto now = std::chrono::steady_clock::now();
  const double elapsedSeconds =
      std::chrono::duration<double>(now - start).count();
  const double percent = total == 0 ? 100.0
                                    : (static_cast<double>(done) * 100.0) /
                                          static_cast<double>(total);
  const double etaSeconds = done == 0
                                ? 0.0
                                : (elapsedSeconds / static_cast<double>(done)) *
                                      static_cast<double>(total - done);

  std::ostringstream percentStream;
  percentStream << std::fixed << std::setprecision(1) << percent;

  llvm::outs() << "\r\033[32mZ3 [" << makeProgressBar(done, total) << "] "
               << percentStream.str() << "% " << done << "/" << total
               << " elapsed=" << formatSeconds(elapsedSeconds)
               << " eta=" << formatSeconds(etaSeconds) << "\033[0m";
  if (done == total) {
    llvm::outs() << "\n";
  }
  llvm::outs().flush();
}

std::optional<std::vector<CompletePathCondition>>
readCompletePathConditions(const std::string &inputFilename) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileOrErr =
      llvm::MemoryBuffer::getFile(inputFilename);
  if (!fileOrErr) {
    llvm::errs() << "Failed to open complete-path input file '"
                 << inputFilename << "': " << fileOrErr.getError().message()
                 << "\n";
    return std::nullopt;
  }

  llvm::Expected<llvm::json::Value> parsed =
      llvm::json::parse(fileOrErr.get()->getBuffer());
  if (!parsed) {
    llvm::errs() << "Failed to parse complete-path JSON '" << inputFilename
                 << "': " << llvm::toString(parsed.takeError()) << "\n";
    return std::nullopt;
  }

  const llvm::json::Object *root = parsed->getAsObject();
  if (!root) {
    llvm::errs() << "Complete-path JSON root must be an object\n";
    return std::nullopt;
  }

  const std::optional<std::int64_t> totalPaths =
      root->getInteger("total_paths");
  const llvm::json::Array *pathArray = root->getArray("path_conditions");
  if (!totalPaths || *totalPaths < 0 || !pathArray) {
    llvm::errs() << "Complete-path JSON requires non-negative 'total_paths' "
                    "and a 'path_conditions' array\n";
    return std::nullopt;
  }

  std::vector<CompletePathCondition> conditions;
  conditions.reserve(pathArray->size());
  for (const llvm::json::Value &value : *pathArray) {
    const llvm::json::Object *pathObject = value.getAsObject();
    if (!pathObject) {
      llvm::errs() << "Each complete-path entry must be an object\n";
      return std::nullopt;
    }

    const std::optional<llvm::StringRef> pathLabel =
        pathObject->getString("path_label");
    const std::optional<llvm::StringRef> predicate =
        pathObject->getString("predicate");
    if (!pathLabel || !predicate || predicate->empty()) {
      llvm::errs() << "Each complete-path entry requires a "
                      "'path_label' and a non-empty 'predicate' string\n";
      return std::nullopt;
    }

    conditions.push_back({pathLabel->str(), predicate->str()});
  }

  if (static_cast<std::int64_t>(conditions.size()) != *totalPaths) {
    llvm::errs() << "Complete-path JSON 'total_paths' does not match the "
                    "number of path conditions\n";
    return std::nullopt;
  }
  return conditions;
}

bool writeSolverResults(
    const std::string &outputFilename,
    const Z3PathConditionSolver::SolverRunSummary &summary) {
  llvm::json::Object definitions{
      {"sat", "At least one input model satisfies the path predicate."},
      {"unsat", "No input model satisfies the path predicate."},
      {"unknown", "The solver could not determine satisfiability."},
      {"unsupported",
       "The path contains an expression that cannot be solved."}};
  llvm::json::Object summaryObject{
      {"input_model", summary.inputModel},
      {"total_paths", static_cast<std::int64_t>(summary.results.size())},
      {"sat", static_cast<std::int64_t>(summary.statusCounts.count("sat")
                                               ? summary.statusCounts.at("sat")
                                               : 0)},
      {"unsat", static_cast<std::int64_t>(summary.statusCounts.count("unsat")
                                                 ? summary.statusCounts.at("unsat")
                                                 : 0)},
      {"unknown",
       static_cast<std::int64_t>(summary.statusCounts.count("unknown")
                                     ? summary.statusCounts.at("unknown")
                                     : 0)},
      {"unsupported",
       static_cast<std::int64_t>(summary.statusCounts.count("unsupported")
                                     ? summary.statusCounts.at("unsupported")
                                     : 0)},
      {"total_solve_time_ms", summary.totalSolveTimeMs},
      {"definitions", std::move(definitions)}};

  llvm::json::Array solverResults;
  for (const auto &result : summary.results) {
    llvm::json::Object resultObject{{"path_label", result.pathLabel},
                                    {"predicate", result.predicate},
                                    {"status", result.status},
                                    {"solve_time_ms", result.solveTimeMs},
                                    {"model_validated", result.modelValidated}};
    if (!result.error.empty()) {
      resultObject["error"] = result.error;
    }
    if (!result.model.empty()) {
      llvm::json::Object model;
      for (const auto &[name, value] : result.model) {
        model[name] = value;
      }
      resultObject["model"] = std::move(model);
    }
    solverResults.push_back(std::move(resultObject));
  }

  llvm::json::Object root{{"summary", std::move(summaryObject)},
                          {"solver_results", std::move(solverResults)}};
  std::error_code error;
  llvm::raw_fd_ostream output(outputFilename, error);
  if (error) {
    llvm::errs() << "Could not open solver output file '" << outputFilename
                 << "': " << error.message() << "\n";
    return false;
  }
  output << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(root)));
  return !output.has_error();
}

#ifdef HAVE_Z3

Z3PathConditionSolver::SolverResult
solveConditionAttempt(const CompletePathCondition &condition,
                      const Z3PathConditionSolver::SolverOptions &options,
                      bool useSmallModelBounds) try {
  Z3PathConditionSolver::SolverResult result;
  result.pathLabel = condition.pathLabel;
  result.predicate = condition.predicate;

  z3::context context;
  z3::solver solver(context, z3::solver::simple{});
  std::string parseError;
  PredicateParser parser(condition.predicate, context, options.inputNames,
                         options.wrapArithmetic);
  const auto expression = parser.parse(parseError);
  if (!parseError.empty()) {
    result.status = "unsupported";
    result.error = parseError;
    return result;
  }

  const bool hasSignature = options.enforceInputSignature || !options.inputNames.empty();
  std::set<std::string> inputNames;
  for (const std::string &name : options.inputNames) {
    const bool validName = !name.empty() &&
        (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
        std::all_of(name.begin(), name.end(), [](unsigned char ch) {
          return std::isalnum(ch) || ch == '_';
        }) && name != "true" && name != "false" && name != "and" && name != "not";
    if (!validName || !inputNames.insert(name).second) {
      result.status = "unsupported";
      result.error = "invalid or duplicate input name '" + name + "'";
      return result;
    }
  }
  if (hasSignature) {
    for (const auto &[name, symbol] : parser.symbols()) {
      (void)symbol;
      if (!inputNames.count(name)) {
        result.status = "unsupported";
        result.error = "predicate references undeclared input '" + name + "'";
        return result;
      }
    }
  }

  const auto numeral = [&](std::int64_t value) {
    return options.wrapArithmetic ? context.bv_val(value, 32) : context.int_val(value);
  };
  solver.add(expression);
  if (useSmallModelBounds || !options.wrapArithmetic) {
    std::int64_t lower = std::numeric_limits<std::int32_t>::min();
    std::int64_t upper = std::numeric_limits<std::int32_t>::max();
    if (useSmallModelBounds) {
      const std::int64_t requested = options.smallModelBound;
      upper = std::min(upper, requested < 0 ? -requested : requested);
      lower = -upper;
    }
    for (const auto &[name, symbol] : parser.symbols()) {
      (void)name;
      solver.add(symbol >= numeral(lower));
      solver.add(symbol <= numeral(upper));
    }
  }

  const auto checkResult = solver.check();
  if (checkResult == z3::unknown) {
    result.status = "unknown";
    return result;
  }
  if (checkResult == z3::unsat) {
    result.status = "unsat";
    return result;
  }

  // Capture the model before adding assertions for the exported input values.
  const auto model = solver.get_model();
  for (const auto &[name, symbol] : parser.symbols()) {
    const auto value = options.wrapArithmetic ? z3::bv2int(symbol, true) : symbol;
    result.model[name] = std::to_string(model.eval(value, true).get_numeral_int64());
  }
  for (const std::string &name : options.inputNames)
    result.model.emplace(name, "0");

  for (const auto &[name, symbol] : parser.symbols())
    solver.add(symbol == numeral(std::stoll(result.model.at(name))));
  const auto validation = solver.check();
  if (validation != z3::sat) {
    result.status = "unknown";
    result.error = validation == z3::unsat
                       ? "exported input model failed formula validation"
                       : "solver could not validate the exported input model";
    result.model.clear();
    return result;
  }
  result.status = "sat";
  result.modelValidated = true;
  return result;
} catch (const z3::exception &error) {
  Z3PathConditionSolver::SolverResult result;
  result.pathLabel = condition.pathLabel;
  result.predicate = condition.predicate;
  result.status = "unsupported";
  result.error = error.msg();
  return result;
}

Z3PathConditionSolver::SolverResult
solveCondition(const CompletePathCondition &condition,
               const Z3PathConditionSolver::SolverOptions &options) {
  const auto start = std::chrono::steady_clock::now();
  Z3PathConditionSolver::SolverResult result =
      solveConditionAttempt(condition, options, options.preferSmallModels);

  if (options.preferSmallModels && result.status == "unsat") {
    Z3PathConditionSolver::SolverResult unboundedResult =
        solveConditionAttempt(condition, options, false);
    if (unboundedResult.status != "unsat") {
      result = std::move(unboundedResult);
    }
  }
  if (options.collectTiming) {
    const auto end = std::chrono::steady_clock::now();
    result.solveTimeMs =
        std::chrono::duration<double, std::milli>(end - start).count();
  }
  return result;
}

#endif // HAVE_Z3

Z3PathConditionSolver::SolverRunSummary
solveCompletePathConditions(const std::vector<CompletePathCondition> &conditions,
                        const Z3PathConditionSolver::SolverOptions &options) {
  Z3PathConditionSolver::SolverRunSummary summary;
  summary.inputModel = options.wrapArithmetic ? "signed-bv32-wrap"
                                              : "mathematical-integers";
  summary.results.reserve(conditions.size());
  const auto start = std::chrono::steady_clock::now();

#ifdef HAVE_Z3
  for (size_t index = 0; index < conditions.size(); ++index) {
    summary.results.push_back(solveCondition(conditions[index], options));
    if (options.progressInterval != 0 &&
        ((index + 1) % options.progressInterval == 0 ||
         index + 1 == conditions.size())) {
      printSolverProgress(index + 1, conditions.size(), start);
    }
  }
#else
  for (const auto &condition : conditions) {
    Z3PathConditionSolver::SolverResult result;
    result.pathLabel = condition.pathLabel;
    result.predicate = condition.predicate;
    result.status = "unsupported";
    result.error = "Z3 support not available";
    summary.results.push_back(std::move(result));
  }
#endif

  if (options.collectTiming) {
    const auto end = std::chrono::steady_clock::now();
    summary.totalSolveTimeMs =
        std::chrono::duration<double, std::milli>(end - start).count();
  }
  for (const auto &result : summary.results) {
    ++summary.statusCounts[result.status];
  }
  return summary;
}

} // namespace

bool Z3PathConditionSolver::isZ3Available() {
#ifdef HAVE_Z3
  return true;
#else
  return false;
#endif
}

Z3PathConditionSolver::SolverRunSummary
Z3PathConditionSolver::solvePaths(
    const std::vector<CompletePathCondition> &conditions,
    const SolverOptions &options) {
  return solveCompletePathConditions(conditions, options);
}

std::optional<Z3PathConditionSolver::SolverRunSummary>
Z3PathConditionSolver::solveFromJSON(const std::string &inputFilename,
                                     const SolverOptions &options) {
  const std::optional<std::vector<CompletePathCondition>> conditions =
      readCompletePathConditions(inputFilename);
  if (!conditions) {
    return std::nullopt;
  }
  return solveCompletePathConditions(*conditions, options);
}

bool Z3PathConditionSolver::writeResults(const std::string &outputFilename,
                                         const SolverRunSummary &summary) {
  return writeSolverResults(outputFilename, summary);
}
