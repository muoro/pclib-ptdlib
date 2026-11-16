#include "gtest/gtest.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/Tooling.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "predicates/analysis/CFGAnalyzer.h"

struct CFGTestParams {
  std::string_view code;
  std::string_view functionName;
  std::vector<std::pair<std::string_view, std::string_view>> expectedResults;
  std::vector<std::string_view> expectedPathLabels;
};

[[nodiscard]] std::string PrintToString(const CFGTestParams &params) noexcept {
  return std::string{params.functionName};
}

class CFGAnalyzerTestHelper {
public:
  static void analyzeAndVerify(const CFGTestParams &params) {
    auto pchContainerOps = std::make_shared<clang::PCHContainerOperations>();

    const std::string codeStr{params.code};
    std::unique_ptr<clang::ASTUnit> ast =
        clang::tooling::buildASTFromCode(codeStr, "input.cc", pchContainerOps);
    ASSERT_TRUE(ast) << "Failed to build AST.";

    clang::ASTContext &context = ast->getASTContext();
    clang::FunctionDecl *functionDecl =
        findFunctionDecl(context, params.functionName);
    ASSERT_NE(functionDecl, nullptr)
        << "FunctionDecl '" << params.functionName << "' not found in AST.";

    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(functionDecl, functionDecl->getBody(), &context,
                             clang::CFG::BuildOptions());
    ASSERT_TRUE(cfg) << "Failed to build CFG for function "
                     << params.functionName;

    CFGAnalyzer cfgAnalyzer(cfg.get());
    cfgAnalyzer.analyze(false);

    const auto *nodes = cfgAnalyzer.getEntryNodes();
    ASSERT_NE(nodes, nullptr);
    verifyResults(*nodes, params);
  }

private:
  [[nodiscard]] static clang::FunctionDecl *
  findFunctionDecl(clang::ASTContext &context, std::string_view functionName) {
    clang::TranslationUnitDecl *translationUnit =
        context.getTranslationUnitDecl();

    for (auto *decl : translationUnit->decls()) {
      if (auto *func = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if (func->hasBody() && func->getNameAsString() == functionName) {
          return func;
        }
      }
    }
    return nullptr;
  }

  static void verifyResults(const PathNodeSet &nodes,
                            const CFGTestParams &params) {
    for (const auto &[labelView, predicateView] : params.expectedResults) {
      const std::string label(labelView);
      const auto *node = nodes.findNode(label);
      ASSERT_NE(node, nullptr) << "Missing node: " << label;
      EXPECT_EQ(node->edgePredicate, std::string(predicateView)) << label;
    }

    std::vector<std::string> actualLabels;
    for (const auto &[label, node] : nodes) {
      if (node.hasStoredEdgePredicate())
        actualLabels.push_back(label);
    }

    std::vector<std::string> expectedLabels;
    expectedLabels.reserve(params.expectedPathLabels.size());
    for (std::string_view label : params.expectedPathLabels) {
      expectedLabels.emplace_back(label);
    }

    std::sort(actualLabels.begin(), actualLabels.end());
    std::sort(expectedLabels.begin(), expectedLabels.end());
    EXPECT_EQ(actualLabels, expectedLabels);
  }
};

class CFGAnalyzerParamTest : public ::testing::TestWithParam<CFGTestParams> {
public:
  void analyzeCode(const CFGTestParams &params) const {
    CFGAnalyzerTestHelper::analyzeAndVerify(params);
  }
};

TEST_P(CFGAnalyzerParamTest, MultipleFunctionsCFGTest) {
  const auto &params = GetParam();
  analyzeCode(params);
}

// Stored predicates are incremental true-edge constraints. Structural false
// nodes are tested separately through PClib and complete-path reconstruction.
INSTANTIATE_TEST_SUITE_P(SimpleIfAssignmentTest, CFGAnalyzerParamTest,
                         ::testing::Values(CFGTestParams{
                             R"(void simple_if_assignment(int a, int b, int c) {
      a = a + 1;
      if (a < b) { c = c + 1; } else { c = c - 1; }
    })",
                             "simple_if_assignment",
                             {{"B3T", "((a + 1) < b)"}},
                             {"B3T"}}));

INSTANTIATE_TEST_SUITE_P(NestedIfTest, CFGAnalyzerParamTest,
                         ::testing::Values(CFGTestParams{
                             R"(void nested_if(int a, int b, int c) {
      a = a + 1;
      if (a < b) {
        b = b + 2;
        if (b < c) { c = c - 1; } else { c = c + 1; }
      }
      if (c < a) { a = a + b; }
    })",
                             "nested_if",
                             {{"B6F.B2T", "(c < (a + 1))"},
                              {"B6T", "((a + 1) < b)"},
                              {"B6T.B5T", "((b + 2) < c)"},
                              {"B6T.B5F.B2T", "((c + 1) < (a + 1))"},
                              {"B6T.B5T.B2T", "((c - 1) < (a + 1))"}},
                             {"B6F.B2T", "B6T", "B6T.B5T", "B6T.B5F.B2T",
                              "B6T.B5T.B2T"}}));

INSTANTIATE_TEST_SUITE_P(LoopWhileTest, CFGAnalyzerParamTest,
                         ::testing::Values(CFGTestParams{
                             R"(void loop_while(int a) {
      while (a <= 10) { a = a + 1; }
    })",
                             "loop_while",
                             {{"B3T", "(a <= 10)"}},
                             {"B3T", "B3T.B3T", "B3T.B3T.B3T"}}));

INSTANTIATE_TEST_SUITE_P(ForLoopTest, CFGAnalyzerParamTest,
                         ::testing::Values(CFGTestParams{
                             R"(void for_loop(int a, int b, int c) {
      for (a = 0; a < (b + c);) { a = a + 1; c = c + 1; }
    })",
                             "for_loop",
                             {{"B3T", "((0) < (b + c))"}},
                             {"B3T", "B3T.B3T", "B3T.B3T.B3T"}}));
