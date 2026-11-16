#include "predicates/library/PTDLibrary.h"
#include "predicates/model/PathLabel.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace {

bool isInputName(const std::string &name) {
  return !name.empty() &&
         (std::isalpha(static_cast<unsigned char>(name[0])) ||
          name[0] == '_') &&
         std::all_of(
             name.begin(), name.end(),
             [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; }) &&
         name != "true" && name != "false" && name != "and" && name != "not";
}

bool validate(const PTDLibrary &library, std::string &error) {
  if (library.inputModel != "mathematical-integers" &&
      library.inputModel != "signed-bv32-wrap" &&
      library.inputModel != "signed-bv32-no-overflow") {
    error = "unsupported PTDlib arithmetic model";
    return false;
  }
  if (library.pclibId.empty() || library.functionName.empty()) {
    error = "PTDlib requires non-empty pclib_id and function";
    return false;
  }
  std::set<std::string> names;
  for (const auto &name : library.inputNames) {
    if (!isInputName(name) || !names.insert(name).second) {
      error = "PTDlib contains an invalid or duplicate input name";
      return false;
    }
  }
  std::map<std::string, const SolverResult *> objectives;
  for (const auto &result : library.diagnostics) {
    if (!PathLabels::isValid(result.pathLabel) || result.predicate.empty() ||
        !std::isfinite(result.solveTimeMs) || result.solveTimeMs < 0 ||
        (result.status != "sat" && result.status != "unsat" &&
         result.status != "unknown" && result.status != "unsupported") ||
        (result.status != "sat" && result.modelValidated) ||
        !objectives.emplace(result.pathLabel, &result).second) {
      error = "PTDlib contains invalid or duplicate objective diagnostics";
      return false;
    }
  }
  std::set<std::string> ids;
  for (const auto &test : library.tests) {
    const auto objective = objectives.find(test.pathLabel);
    if (test.id.empty() || !ids.insert(test.id).second ||
        objective == objectives.end() || objective->second->status != "sat" ||
        !objective->second->modelValidated) {
      error = "PTDlib test requires a unique ID and a formula-validated SAT "
              "objective";
      return false;
    }
    if (test.inputs.size() != names.size()) {
      error = "PTDlib test input vector does not match the declared signature";
      return false;
    }
    for (const auto &[name, value] : test.inputs) {
      (void)value;
      if (!names.count(name)) {
        error = "PTDlib test contains an undeclared input";
        return false;
      }
    }
  }
  return true;
}

std::optional<std::int32_t> parseInput(const std::string &value) {
  std::int32_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

} // namespace

std::optional<PTDLibrary> PTDLibrary::fromSolverResults(
    const std::string &pclibId, const std::string &functionName,
    const std::vector<std::string> &inputNames,
    const SolverRunSummary &summary,
    std::string &error) {
  error.clear();
  PTDLibrary library;
  library.inputModel = summary.inputModel;
  library.pclibId = pclibId;
  library.functionName = functionName;
  library.inputNames = inputNames;
  library.diagnostics = summary.results;
  for (const auto &result : summary.results) {
    if (result.status != "sat") {
      continue;
    }
    if (!result.modelValidated) {
      error = "cannot export an unvalidated SAT model to PTDlib";
      return std::nullopt;
    }
    TestInput test;
    test.id = "t" + std::to_string(library.tests.size() + 1);
    test.pathLabel = result.pathLabel;
    for (const auto &[name, value] : result.model) {
      const auto parsed = parseInput(value);
      if (!parsed) {
        error = "solver model is not a signed 32-bit input vector";
        return std::nullopt;
      }
      test.inputs.emplace(name, *parsed);
    }
    library.tests.push_back(std::move(test));
  }
  if (!validate(library, error)) {
    return std::nullopt;
  }
  return library;
}

bool PTDLibrary::writeFromSolverResults(
    const std::string &filename, const std::string &pclibId,
    const std::string &functionName, const std::vector<std::string> &inputNames,
    const SolverRunSummary &summary,
    std::string &error) {
  const auto library =
      fromSolverResults(pclibId, functionName, inputNames, summary, error);
  return library && library->writeJSON(filename, error);
}

bool PTDLibrary::writeJSON(const std::string &filename,
                           std::string &error) const {
  error.clear();
  if (!validate(*this, error)) {
    return false;
  }
  std::set<std::string> objectivesWithTests;
  for (const auto &test : tests) {
    objectivesWithTests.insert(test.pathLabel);
  }
  std::error_code ec;
  llvm::raw_fd_ostream output(filename, ec);
  if (ec) {
    error = "cannot write PTDlib: " + ec.message();
    return false;
  }
  // Keep the requested reading order without relying on json::Object's sorted
  // keys. Serialize all dynamic keys and values through the proven formatv
  // path, as in PCLibrary; direct OStream construction fails with this LLVM build.
  const auto field = [&output](unsigned indent, llvm::StringRef name,
                               const llvm::json::Value &value, bool comma) {
    const std::string encoded = llvm::formatv("{0:2}", value).str();
    output.indent(indent);
    output << llvm::formatv("{0}", llvm::json::Value(name)) << ": ";
    for (char ch : encoded) {
      output << ch;
      if (ch == '\n') {
        output.indent(indent);
      }
    }
    output << (comma ? ",\n" : "\n");
  };
  output << "{\n  \"summary\": {\n";
  field(4, "total_tests", static_cast<std::int64_t>(tests.size()), true);
  field(4, "total_objectives", static_cast<std::int64_t>(diagnostics.size()), true);
  field(4, "objectives_with_tests",
        static_cast<std::int64_t>(objectivesWithTests.size()), false);
  output << "  },\n  \"metadata\": {\n";
  field(4, "schema", "ptdlib", true);
  field(4, "schema_version", 2, true);
  field(4, "pclib_id", pclibId, false);
  output << "  },\n  \"analysis\": {\n";
  field(4, "input_model", inputModel, false);
  output << "  },\n  \"function\": {\n";
  field(4, "name", functionName, true);
  llvm::json::Array signature;
  for (const auto &name : inputNames) {
    signature.push_back(llvm::json::Object{{"name", name}, {"type", "int32"}});
  }
  field(4, "inputs", std::move(signature), false);
  output << "  },\n  \"tests\": [";
  bool firstTest = true;
  for (const auto &test : tests) {
    output << (firstTest ? "\n" : ",\n");
    firstTest = false;
    output << "    {\n";
    field(6, "id", test.id, true);
    field(6, "path_label", test.pathLabel, true);
    llvm::json::Object inputs;
    for (const auto &[name, value] : test.inputs) {
      inputs[name] = static_cast<std::int64_t>(value);
    }
    field(6, "inputs", std::move(inputs), true);
    field(6, "generator", "z3", true);
    field(6, "validation", "smt_model", true);
    field(6, "execution_verified", false, false);
    output << "    }";
  }
  output << (tests.empty() ? "],\n" : "\n  ],\n");
  output << "  \"diagnostics\": [";
  bool firstDiagnostic = true;
  for (const auto &result : diagnostics) {
    output << (firstDiagnostic ? "\n" : ",\n");
    firstDiagnostic = false;
    output << "    {\n";
    field(6, "path_label", result.pathLabel, true);
    field(6, "status", result.status, true);
    field(6, "predicate", result.predicate, true);
    field(6, "model_validated", result.modelValidated, true);
    field(6, "solve_time_ms", result.solveTimeMs, !result.error.empty());
    if (!result.error.empty()) {
      field(6, "error", result.error, false);
    }
    output << "    }";
  }
  output << (diagnostics.empty() ? "]\n}\n" : "\n  ]\n}\n");
  output.close();
  if (output.has_error()) {
    error = "failed to write PTDlib";
    return false;
  }
  return true;
}

std::optional<PTDLibrary> PTDLibrary::readJSON(const std::string &filename,
                                               std::string &error) {
  error.clear();
  auto buffer = llvm::MemoryBuffer::getFile(filename);
  if (!buffer) {
    error = "cannot read PTDlib: " + buffer.getError().message();
    return std::nullopt;
  }
  auto value = llvm::json::parse((*buffer)->getBuffer());
  if (!value) {
    error = "invalid PTDlib JSON: " + llvm::toString(value.takeError());
    return std::nullopt;
  }
  const auto *root = value->getAsObject();
  if (!root) {
    error = "PTDlib must be a JSON object";
    return std::nullopt;
  }
  const bool grouped = root->get("metadata") != nullptr;
  const llvm::json::Object *metadata = root;
  const llvm::json::Object *analysis = root;
  const llvm::json::Object *functionObject = nullptr;
  if (grouped) {
    metadata = root->getObject("metadata");
    analysis = root->getObject("analysis");
    functionObject = root->getObject("function");
    if (!metadata || !analysis || !functionObject) {
      error = "PTDlib v2 requires metadata, analysis and function objects";
      return std::nullopt;
    }
    for (const char *key : {"schema", "schema_version", "pclib_id",
                            "input_model", "input_names"}) {
      if (root->get(key)) {
        error = "PTDlib cannot mix flat v1 and grouped v2 fields";
        return std::nullopt;
      }
    }
  } else if (root->get("analysis")) {
    error = "PTDlib cannot mix flat v1 and grouped v2 fields";
    return std::nullopt;
  }
  if (metadata->getString("schema") != "ptdlib" ||
      metadata->getInteger("schema_version") != (grouped ? 2 : 1) ||
      (analysis->getString("input_model") != "mathematical-integers" &&
       analysis->getString("input_model") != "signed-bv32-wrap" &&
       analysis->getString("input_model") != "signed-bv32-no-overflow")) {
    error = "unsupported PTDlib schema, version or input model";
    return std::nullopt;
  }
  const auto pclibId = metadata->getString("pclib_id");
  const auto function = grouped ? functionObject->getString("name")
                                : root->getString("function");
  const auto *names = grouped ? functionObject->getArray("inputs")
                             : root->getArray("input_names");
  const auto *tests = root->getArray("tests");
  const auto *diagnostics = root->getArray("diagnostics");
  const auto *summary = root->getObject("summary");
  if (!pclibId || !function || !names || !tests || !diagnostics || !summary) {
    error = "PTDlib is missing required fields";
    return std::nullopt;
  }
  const char *objectivesCountKey = grouped ? "objectives_with_tests"
                                          : "covered_objectives";
  if (summary->get(grouped ? "covered_objectives" : "objectives_with_tests")) {
    error = "PTDlib cannot mix v1 and v2 summary fields";
    return std::nullopt;
  }
  for (const char *key : {"total_tests", "total_objectives", objectivesCountKey}) {
    const auto count = summary->getInteger(key);
    if (!count || *count < 0) {
      error = "PTDlib summary counts must be nonnegative integers";
      return std::nullopt;
    }
  }
  PTDLibrary library;
  library.inputModel = analysis->getString("input_model")->str();
  library.pclibId = pclibId->str();
  library.functionName = function->str();
  for (const auto &name : *names) {
    if (grouped) {
      const auto *input = name.getAsObject();
      if (!input || !input->getString("name") ||
          input->getString("type") != "int32") {
        error = "PTDlib inputs must be named int32 values";
        return std::nullopt;
      }
      library.inputNames.push_back(input->getString("name")->str());
    } else {
      const auto text = name.getAsString();
      if (!text) {
        error = "PTDlib input names must be strings";
        return std::nullopt;
      }
      library.inputNames.push_back(text->str());
    }
  }
  std::set<std::string> covered;
  for (const auto &entry : *tests) {
    const auto *test = entry.getAsObject();
    if (!test || !test->getString("id") || !test->getString("path_label") ||
        !test->getObject("inputs") || test->getString("generator") != "z3" ||
        test->getString("validation") != "smt_model" ||
        test->getBoolean("execution_verified") != false) {
      error = "invalid PTDlib test record";
      return std::nullopt;
    }
    TestInput parsed;
    parsed.id = test->getString("id")->str();
    parsed.pathLabel = test->getString("path_label")->str();
    for (const auto &[name, item] : *test->getObject("inputs")) {
      const auto integer = item.getAsInteger();
      if (!integer || *integer < std::numeric_limits<std::int32_t>::min() ||
          *integer > std::numeric_limits<std::int32_t>::max()) {
        error = "PTDlib input values must be signed 32-bit integers";
        return std::nullopt;
      }
      parsed.inputs.emplace(name.str(), static_cast<std::int32_t>(*integer));
    }
    covered.insert(parsed.pathLabel);
    library.tests.push_back(std::move(parsed));
  }
  for (const auto &entry : *diagnostics) {
    const auto *diagnostic = entry.getAsObject();
    if (!diagnostic || !diagnostic->getString("path_label") ||
        !diagnostic->getString("predicate") ||
        !diagnostic->getString("status") ||
        !diagnostic->getBoolean("model_validated").has_value() ||
        !diagnostic->getNumber("solve_time_ms")) {
      error = "invalid PTDlib diagnostic record";
      return std::nullopt;
    }
    SolverResult result;
    result.pathLabel = diagnostic->getString("path_label")->str();
    result.predicate = diagnostic->getString("predicate")->str();
    result.status = diagnostic->getString("status")->str();
    result.modelValidated = *diagnostic->getBoolean("model_validated");
    result.solveTimeMs = *diagnostic->getNumber("solve_time_ms");
    if (const auto message = diagnostic->getString("error")) {
      result.error = message->str();
    }
    library.diagnostics.push_back(std::move(result));
  }
  if (summary->getInteger("total_tests") !=
          static_cast<std::int64_t>(library.tests.size()) ||
      summary->getInteger("total_objectives") !=
          static_cast<std::int64_t>(library.diagnostics.size()) ||
      summary->getInteger(objectivesCountKey) !=
          static_cast<std::int64_t>(covered.size())) {
    error = "PTDlib summary counts do not match its records";
    return std::nullopt;
  }
  if (!validate(library, error)) {
    return std::nullopt;
  }
  return library;
}
