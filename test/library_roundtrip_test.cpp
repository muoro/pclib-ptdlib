#include "gtest/gtest.h"

#include "predicates/export/CompletePathConditionBuilder.h"
#include "predicates/library/PTDLibrary.h"
#include "predicates/solver/Z3PathConditionSolver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

class TemporaryJSON {
public:
  bool create() {
    return !llvm::sys::fs::createTemporaryFile("pccscc-ptdlib-test", "json",
                                               path_);
  }
  ~TemporaryJSON() {
    if (!path_.empty()) {
      (void)llvm::sys::fs::remove(path_);
    }
  }
  std::string path() const { return path_.str().str(); }

private:
  llvm::SmallString<128> path_;
};

std::string readText(const std::string &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool writeText(const std::string &path, const std::string &text) {
  std::ofstream output(path);
  output << text;
  output.close();
  return !output.fail();
}

llvm::json::Value groupedPTDFixture() {
  // Independent grouped input; its member order is deliberately not the
  // human-facing writer order. Reading does not constitute model verification.
  auto parsed = llvm::json::parse(R"json({
    "diagnostics":[{"status":"sat","path_label":"B1T",
      "predicate":"(a > 0)","model_validated":true,"solve_time_ms":1.25}],
    "tests":[{"inputs":{"a":7,"b":9},"path_label":"B1T","id":"legacy-test",
      "generator":"z3","execution_verified":false,"validation":"smt_model"}],
    "function":{"inputs":[{"name":"a","type":"int32"},
      {"name":"b","type":"int32"}],"name":"f"},
    "analysis":{"input_model":"signed-bv32-wrap"},
    "metadata":{"pclib_id":"pclib-fixture","schema_version":2,"schema":"ptdlib"},
    "summary":{"objectives_with_tests":1,"total_objectives":1,"total_tests":1}
  })json");
  if (!parsed) {
    ADD_FAILURE() << llvm::toString(parsed.takeError());
    return nullptr;
  }
  return std::move(*parsed);
}

void expectGroupedOrder(const std::string &text) {
  size_t previous = 0;
  bool first = true;
  for (const std::string &key :
       {"summary", "metadata", "analysis", "function", "tests", "diagnostics"}) {
    const auto position = text.find("\n  \"" + key + "\":");
    ASSERT_NE(position, std::string::npos) << key;
    if (!first) {
      EXPECT_GT(position, previous) << key;
    }
    first = false;
    previous = position;
  }
}

Z3PathConditionSolver::SolverRunSummary rootSummary() {
  Z3PathConditionSolver::SolverResult result;
  result.pathLabel = "";
  result.predicate = "true";
  result.status = "sat";
  result.model = {{"a", "-2147483648"}, {"b", "2147483647"}};
  result.modelValidated = true;
  Z3PathConditionSolver::SolverRunSummary summary;
  summary.results.push_back(std::move(result));
  summary.statusCounts["sat"] = 1;
  return summary;
}

TEST(PTDLibraryTest, RootAndInt32BoundaryInputsRoundTrip) {
  std::string error;
  const auto library = PTDLibrary::fromSolverResults(
      "pclib-fixture", "branchless", {"a", "b"}, rootSummary(), error);
  ASSERT_TRUE(library) << error;
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  ASSERT_TRUE(library->writeJSON(file.path(), error)) << error;
  const auto restored = PTDLibrary::readJSON(file.path(), error);
  ASSERT_TRUE(restored) << error;
  EXPECT_EQ(restored->pclibId, "pclib-fixture");
  EXPECT_EQ(restored->functionName, "branchless");
  EXPECT_EQ(restored->inputNames, (std::vector<std::string>{"a", "b"}));
  ASSERT_EQ(restored->tests.size(), 1U);
  EXPECT_EQ(restored->tests[0].pathLabel, "");
  EXPECT_EQ(restored->tests[0].inputs.at("a"),
            std::numeric_limits<std::int32_t>::min());
  EXPECT_EQ(restored->tests[0].inputs.at("b"),
            std::numeric_limits<std::int32_t>::max());
  ASSERT_EQ(restored->diagnostics.size(), 1U);
  EXPECT_EQ(restored->diagnostics[0].status, "sat");
  EXPECT_TRUE(restored->diagnostics[0].modelValidated);
  // A structural roundtrip preserves the recorded validation metadata. It does
  // not assert that reading performed a solver check or native execution.
}

TEST(PTDLibraryTest, MultipleTestsCanTargetOneObjective) {
  std::string error;
  auto library = PTDLibrary::fromSolverResults(
      "pclib-fixture", "branchless", {"a", "b"}, rootSummary(), error);
  ASSERT_TRUE(library) << error;
  PTDLibrary::TestInput second = library->tests.front();
  second.id = "t2";
  second.inputs = {{"a", 0}, {"b", 1}};
  library->tests.push_back(std::move(second));
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  ASSERT_TRUE(library->writeJSON(file.path(), error)) << error;
  auto restored = PTDLibrary::readJSON(file.path(), error);
  ASSERT_TRUE(restored) << error;
  ASSERT_EQ(restored->tests.size(), 2U);
  EXPECT_EQ(restored->tests[0].pathLabel, restored->tests[1].pathLabel);
  EXPECT_NE(restored->tests[0].inputs, restored->tests[1].inputs);
  EXPECT_EQ(restored->diagnostics.size(), 1U);
  auto parsed = llvm::json::parse(readText(file.path()));
  ASSERT_TRUE(static_cast<bool>(parsed))
      << llvm::toString(parsed.takeError());
  const auto *summary = parsed->getAsObject()->getObject("summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->getInteger("total_tests"), 2);
  EXPECT_EQ(summary->getInteger("total_objectives"), 1);
  EXPECT_EQ(summary->getInteger("objectives_with_tests"), 1);
  EXPECT_EQ(summary->get("covered_objectives"), nullptr);
}

TEST(PTDLibraryTest, RejectsUnvalidatedIncompleteOrInvalidModels) {
  std::string error;
  auto summary = rootSummary();
  summary.results[0].modelValidated = false;
  EXPECT_FALSE(PTDLibrary::fromSolverResults("pclib-fixture", "f", {"a", "b"},
                                             summary, error));
  EXPECT_FALSE(error.empty());
  summary = rootSummary();
  summary.results[0].model.erase("b");
  EXPECT_FALSE(PTDLibrary::fromSolverResults("pclib-fixture", "f", {"a", "b"},
                                             summary, error));
  summary = rootSummary();
  summary.results[0].model["extra"] = "0";
  EXPECT_FALSE(PTDLibrary::fromSolverResults("pclib-fixture", "f", {"a", "b"},
                                             summary, error));
  summary = rootSummary();
  summary.results[0].model["a"] = "2147483648";
  EXPECT_FALSE(PTDLibrary::fromSolverResults("pclib-fixture", "f", {"a", "b"},
                                             summary, error));
  summary = rootSummary();
  summary.results[0].model["a"] = "0junk";
  EXPECT_FALSE(PTDLibrary::fromSolverResults("pclib-fixture", "f", {"a", "b"},
                                             summary, error));
}

TEST(PTDLibraryTest, NonSatObjectivesAreDiagnosticsOnly) {
  Z3PathConditionSolver::SolverRunSummary summary;
  for (const auto &[label, status] :
       std::vector<std::pair<std::string, std::string>>{
           {"B1T", "unsat"}, {"B1F", "unknown"}, {"B1T.B2T", "unsupported"}}) {
    Z3PathConditionSolver::SolverResult result;
    result.pathLabel = label;
    result.predicate = "false";
    result.status = status;
    summary.results.push_back(std::move(result));
  }
  std::string error;
  auto library =
      PTDLibrary::fromSolverResults("pclib-fixture", "f", {}, summary, error);
  ASSERT_TRUE(library) << error;
  EXPECT_TRUE(library->tests.empty());
  EXPECT_EQ(library->diagnostics.size(), 3U);
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  ASSERT_TRUE(library->writeJSON(file.path(), error)) << error;
  EXPECT_TRUE(PTDLibrary::readJSON(file.path(), error)) << error;
}

TEST(PTDLibraryTest, ReaderRejectsMalformedSchemaAndOutOfRangeInput) {
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  {
    std::ofstream out(file.path());
    out << R"({"schema":"ptdlib","schema_version":2})";
  }
  std::string error;
  EXPECT_FALSE(PTDLibrary::readJSON(file.path(), error));
  {
    std::ofstream out(file.path());
    out << R"({"schema":"ptdlib","schema_version":1,
      "pclib_id":"fixture","function":"f","input_model":"signed-bv32-wrap",
      "input_names":["a"],"tests":[{"id":"t1","path_label":"",
      "inputs":{"a":2147483648},"generator":"z3","validation":"smt_model",
      "execution_verified":false}],"diagnostics":[{"path_label":"",
      "predicate":"true","status":"sat","model_validated":true,"solve_time_ms":0}],
      "summary":{"total_tests":1,"covered_objectives":1,"total_objectives":1}})";
  }
  EXPECT_FALSE(PTDLibrary::readJSON(file.path(), error));
  EXPECT_NE(error.find("signed 32-bit"), std::string::npos);
}

TEST(PTDLibraryTest, GroupedWriterUsesReadableOrderAndTypedSignature) {
  std::string error;
  const auto library = PTDLibrary::fromSolverResults(
      "pclib-fixture", "branchless", {"b", "a"}, rootSummary(), error);
  ASSERT_TRUE(library) << error;
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  ASSERT_TRUE(library->writeJSON(file.path(), error)) << error;
  const auto text = readText(file.path());
  expectGroupedOrder(text);
  auto parsed = llvm::json::parse(text);
  ASSERT_TRUE(static_cast<bool>(parsed))
      << llvm::toString(parsed.takeError());
  const auto *root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const auto *metadata = root->getObject("metadata");
  ASSERT_NE(metadata, nullptr);
  EXPECT_EQ(metadata->getString("schema"), "ptdlib");
  EXPECT_EQ(metadata->getInteger("schema_version"), 2);
  EXPECT_EQ(metadata->getString("pclib_id"), "pclib-fixture");
  ASSERT_NE(root->getObject("analysis"), nullptr);
  EXPECT_EQ(root->getObject("analysis")->getString("input_model"),
            "mathematical-integers");
  const auto *function = root->getObject("function");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(function->getString("name"), "branchless");
  const auto *inputs = function->getArray("inputs");
  ASSERT_NE(inputs, nullptr);
  ASSERT_EQ(inputs->size(), 2U);
  ASSERT_NE((*inputs)[0].getAsObject(), nullptr);
  ASSERT_NE((*inputs)[1].getAsObject(), nullptr);
  EXPECT_EQ((*inputs)[0].getAsObject()->getString("name"), "b");
  EXPECT_EQ((*inputs)[1].getAsObject()->getString("name"), "a");
  EXPECT_EQ((*inputs)[0].getAsObject()->getString("type"), "int32");
  EXPECT_EQ((*inputs)[1].getAsObject()->getString("type"), "int32");
  for (const char *oldField : {"schema", "schema_version", "pclib_id",
                               "input_model", "input_names"}) {
    EXPECT_EQ(root->get(oldField), nullptr) << oldField;
  }
}

TEST(PTDLibraryTest, LegacyVersionOneUpgradesWithoutLosingRecords) {
  TemporaryJSON original;
  TemporaryJSON rewritten;
  ASSERT_TRUE(original.create());
  ASSERT_TRUE(rewritten.create());
  ASSERT_TRUE(writeText(original.path(), R"json({
    "schema":"ptdlib","schema_version":1,"pclib_id":"pclib-fixture",
    "function":"f","input_model":"signed-bv32-wrap","input_names":["b","a"],
    "tests":[{"id":"old-test","path_label":"B1T","inputs":{"a":7,"b":9},
      "generator":"z3","validation":"smt_model","execution_verified":false}],
    "diagnostics":[
      {"path_label":"B1T","predicate":"(a > 0)","status":"sat",
        "model_validated":true,"solve_time_ms":12.5},
      {"path_label":"B1F","predicate":"not((a > 0))","status":"unknown",
        "model_validated":false,"solve_time_ms":0.5,"error":"solver timed out"}],
    "summary":{"total_tests":1,"total_objectives":2,"covered_objectives":1}
  })json"));
  std::string error;
  const auto legacy = PTDLibrary::readJSON(original.path(), error);
  ASSERT_TRUE(legacy) << error;
  EXPECT_EQ(legacy->pclibId, "pclib-fixture");
  EXPECT_EQ(legacy->functionName, "f");
  EXPECT_EQ(legacy->inputNames, (std::vector<std::string>{"b", "a"}));
  ASSERT_TRUE(legacy->writeJSON(rewritten.path(), error)) << error;
  expectGroupedOrder(readText(rewritten.path()));
  const auto restored = PTDLibrary::readJSON(rewritten.path(), error);
  ASSERT_TRUE(restored) << error;
  EXPECT_EQ(restored->pclibId, legacy->pclibId);
  EXPECT_EQ(restored->functionName, legacy->functionName);
  EXPECT_EQ(restored->inputNames, legacy->inputNames);
  ASSERT_EQ(restored->tests.size(), 1U);
  EXPECT_EQ(restored->tests[0].id, "old-test");
  EXPECT_EQ(restored->tests[0].pathLabel, "B1T");
  EXPECT_EQ(restored->tests[0].inputs,
            (std::map<std::string, std::int32_t>{{"a", 7}, {"b", 9}}));
  ASSERT_EQ(restored->diagnostics.size(), 2U);
  for (size_t i = 0; i < legacy->diagnostics.size(); ++i) {
    EXPECT_EQ(restored->diagnostics[i].pathLabel, legacy->diagnostics[i].pathLabel);
    EXPECT_EQ(restored->diagnostics[i].predicate, legacy->diagnostics[i].predicate);
    EXPECT_EQ(restored->diagnostics[i].status, legacy->diagnostics[i].status);
    EXPECT_EQ(restored->diagnostics[i].modelValidated,
              legacy->diagnostics[i].modelValidated);
    EXPECT_DOUBLE_EQ(restored->diagnostics[i].solveTimeMs,
                     legacy->diagnostics[i].solveTimeMs);
    EXPECT_EQ(restored->diagnostics[i].error, legacy->diagnostics[i].error);
  }
  const auto text = readText(rewritten.path());
  EXPECT_EQ(text.find("covered_objectives"), std::string::npos);
  EXPECT_NE(text.find("objectives_with_tests"), std::string::npos);
  EXPECT_NE(text.find("\"execution_verified\": false"), std::string::npos);
}

TEST(PTDLibraryTest, GroupedReaderAcceptsAnyObjectMemberOrder) {
  TemporaryJSON source;
  TemporaryJSON rewritten;
  ASSERT_TRUE(source.create());
  ASSERT_TRUE(rewritten.create());
  auto fixture = groupedPTDFixture();
  ASSERT_NE(fixture.getAsObject(), nullptr);
  // formatv sorts members alphabetically, deliberately differing from the
  // writer's summary-first convention.
  const auto shuffled = llvm::formatv("{0:2}", fixture).str();
  ASSERT_TRUE(writeText(source.path(), shuffled));
  std::string error;
  const auto library = PTDLibrary::readJSON(source.path(), error);
  ASSERT_TRUE(library) << error;
  ASSERT_EQ(library->tests.size(), 1U);
  EXPECT_EQ(library->tests[0].inputs.at("a"), 7);
  EXPECT_EQ(library->tests[0].inputs.at("b"), 9);
  EXPECT_EQ(library->tests[0].id, "legacy-test");
  ASSERT_TRUE(library->writeJSON(rewritten.path(), error)) << error;
  expectGroupedOrder(readText(rewritten.path()));
  const auto restored = PTDLibrary::readJSON(rewritten.path(), error);
  ASSERT_TRUE(restored) << error;
  EXPECT_EQ(restored->pclibId, library->pclibId);
  EXPECT_EQ(restored->inputNames, library->inputNames);
}

TEST(PTDLibraryTest, GroupedReaderRejectsInvalidCountsSignatureAndMixedLayout) {
  using Mutation = std::pair<std::string,
                             std::function<void(llvm::json::Object &)>>;
  std::vector<Mutation> mutations;
  for (const std::string field :
       {"total_tests", "total_objectives", "objectives_with_tests"}) {
    mutations.push_back({field + " mismatch", [field](llvm::json::Object &root) {
      (*root.getObject("summary"))[field] = 2;
    }});
    mutations.push_back({field + " negative", [field](llvm::json::Object &root) {
      (*root.getObject("summary"))[field] = -1;
    }});
    mutations.push_back({field + " boolean", [field](llvm::json::Object &root) {
      (*root.getObject("summary"))[field] = true;
    }});
    mutations.push_back({field + " string", [field](llvm::json::Object &root) {
      (*root.getObject("summary"))[field] = "1";
    }});
    mutations.push_back({field + " fraction", [field](llvm::json::Object &root) {
      (*root.getObject("summary"))[field] = 1.5;
    }});
    mutations.push_back({field + " missing", [field](llvm::json::Object &root) {
      root.getObject("summary")->erase(field);
    }});
  }
  mutations.push_back({"summary missing", [](llvm::json::Object &root) {
    root.erase("summary");
  }});
  mutations.push_back({"version", [](llvm::json::Object &root) {
    (*root.getObject("metadata"))["schema_version"] = 99;
  }});
  mutations.push_back({"model", [](llvm::json::Object &root) {
    (*root.getObject("analysis"))["input_model"] = "unbounded-integer";
  }});
  mutations.push_back({"mixed layout", [](llvm::json::Object &root) {
    root["pclib_id"] = "flat-fixture";
  }});
  mutations.push_back({"input type", [](llvm::json::Object &root) {
    auto *input = (*root.getObject("function")->getArray("inputs"))[0].getAsObject();
    (*input)["type"] = "uint32";
  }});
  mutations.push_back({"input signature", [](llvm::json::Object &root) {
    auto *input = (*root.getObject("function")->getArray("inputs"))[0].getAsObject();
    (*input)["name"] = "missing";
  }});
  mutations.push_back({"execution flag", [](llvm::json::Object &root) {
    (*(*root.getArray("tests"))[0].getAsObject())["execution_verified"] = true;
  }});
  mutations.push_back({"non-SAT test", [](llvm::json::Object &root) {
    auto *diagnostic = (*root.getArray("diagnostics"))[0].getAsObject();
    (*diagnostic)["status"] = "unsat";
    (*diagnostic)["model_validated"] = false;
  }});
  TemporaryJSON file;
  ASSERT_TRUE(file.create());
  for (const auto &mutation : mutations) {
    SCOPED_TRACE(mutation.first);
    auto fixture = groupedPTDFixture();
    ASSERT_NE(fixture.getAsObject(), nullptr);
    mutation.second(*fixture.getAsObject());
    ASSERT_TRUE(writeText(file.path(), llvm::formatv("{0:2}", fixture).str()));
    std::string error;
    EXPECT_FALSE(PTDLibrary::readJSON(file.path(), error));
    EXPECT_FALSE(error.empty());
  }
}

#ifdef HAVE_Z3
TEST(LibrarySolverTest, CompletesUnconstrainedInputsAndValidatesModels) {
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"a", "b"};
  options.enforceInputSignature = true;
  const auto summary =
      Z3PathConditionSolver::solvePaths({{"", "true"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  EXPECT_EQ(summary.results[0].status, "sat");
  EXPECT_TRUE(summary.results[0].modelValidated);
  EXPECT_EQ(summary.results[0].model,
            (std::map<std::string, std::string>{{"a", "0"}, {"b", "0"}}));
}

TEST(LibrarySolverTest, EnforcedEmptySignatureRejectsUndeclaredSymbols) {
  Z3PathConditionSolver::SolverOptions options;
  options.enforceInputSignature = true;
  auto summary = Z3PathConditionSolver::solvePaths({{"B1T", "a > 0"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  EXPECT_EQ(summary.results[0].status, "unsupported");
  EXPECT_TRUE(summary.results[0].model.empty());
  EXPECT_FALSE(summary.results[0].modelValidated);
  summary = Z3PathConditionSolver::solvePaths({{"", "true"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  EXPECT_EQ(summary.results[0].status, "sat");
  EXPECT_TRUE(summary.results[0].modelValidated);
  EXPECT_TRUE(summary.results[0].model.empty());
}

TEST(LibrarySolverTest, RejectsSymbolsOutsideNonEmptySignature) {
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"a"};
  options.enforceInputSignature = true;
  const auto summary =
      Z3PathConditionSolver::solvePaths({{"B1T", "b > 0"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  EXPECT_EQ(summary.results[0].status, "unsupported");
  EXPECT_NE(summary.results[0].error.find("undeclared"), std::string::npos);
}

TEST(LibrarySolverTest, SmallModelPreferenceDoesNotDiscardLargeSolutions) {
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"a"};
  options.enforceInputSignature = true;
  const auto summary =
      Z3PathConditionSolver::solvePaths({{"B1T", "a > 1000"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  EXPECT_EQ(summary.results[0].status, "sat");
  EXPECT_TRUE(summary.results[0].modelValidated);
  ASSERT_TRUE(summary.results[0].model.count("a"));
  EXPECT_GT(std::stoll(summary.results[0].model.at("a")), 1000);
}
TEST(LibrarySolverTest, SupportsUnaryPlusOnArithmeticValues) {
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"a"};
  options.enforceInputSignature = true;
  const auto summary = Z3PathConditionSolver::solvePaths(
      {{"B1T", "(+a) > 0"}, {"B1F", "not((+a) > 0)"}}, options);
  ASSERT_EQ(summary.results.size(), 2U);
  for (const auto &result : summary.results) {
    ASSERT_EQ(result.status, "sat") << result.error;
    EXPECT_TRUE(result.modelValidated);
    const auto a = std::stoll(result.model.at("a"));
    EXPECT_EQ(a > 0, result.pathLabel == "B1T");
  }
  const auto invalid =
      Z3PathConditionSolver::solvePaths({{"B1T", "+(a > 0)"}}, options);
  ASSERT_EQ(invalid.results.size(), 1U);
  EXPECT_EQ(invalid.results[0].status, "unsupported");
}

TEST(LibrarySolverTest, DeclaredInputMayHaveBranchLabelSpelling) {
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"B3T"};
  options.enforceInputSignature = true;
  const auto summary =
      Z3PathConditionSolver::solvePaths({{"B1T", "B3T > 0"}}, options);
  ASSERT_EQ(summary.results.size(), 1U);
  ASSERT_EQ(summary.results[0].status, "sat") << summary.results[0].error;
  EXPECT_TRUE(summary.results[0].modelValidated);
  EXPECT_GT(std::stoll(summary.results[0].model.at("B3T")), 0);

  // Legacy untyped input still rejects unresolved structural label references.
  const auto legacy = Z3PathConditionSolver::solvePaths({{"B1T", "B3T > 0"}});
  ASSERT_EQ(legacy.results.size(), 1U);
  EXPECT_EQ(legacy.results[0].status, "unsupported");
  EXPECT_NE(legacy.results[0].error.find("unexpanded path label"),
            std::string::npos);
}
#endif

} // namespace

TEST(LibrarySolverTest, MathematicalIntermediateCanExceedInputRange) {
  if (!Z3PathConditionSolver::isZ3Available()) GTEST_SKIP() << "Z3 unavailable";
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"x"};
  const auto result = Z3PathConditionSolver::solvePaths(
      {{"B1T", "(x + 1) > 2147483647"}}, options);
  ASSERT_EQ(result.results.size(), 1u);
  ASSERT_EQ(result.results[0].status, "sat") << result.results[0].error;
  EXPECT_EQ(result.inputModel, "mathematical-integers");
  EXPECT_EQ(result.results[0].model.at("x"), "2147483647");
  EXPECT_TRUE(result.results[0].modelValidated);
}

TEST(LibrarySolverTest, MathematicalInputDomainIsSignedInt32) {
  if (!Z3PathConditionSolver::isZ3Available()) GTEST_SKIP() << "Z3 unavailable";
  Z3PathConditionSolver::SolverOptions options;
  options.inputNames = {"x"};
  const auto result = Z3PathConditionSolver::solvePaths(
      {{"B1T", "x == -2147483648"}, {"B2T", "x > 2147483647"},
       {"B3T", "x < -2147483648"}}, options);
  ASSERT_EQ(result.results.size(), 3u);
  ASSERT_EQ(result.results[0].status, "sat");
  EXPECT_EQ(result.results[0].model.at("x"), "-2147483648");
  EXPECT_EQ(result.results[1].status, "unsat");
  EXPECT_EQ(result.results[2].status, "unsat");
}
