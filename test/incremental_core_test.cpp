#include "gtest/gtest.h"

#include "predicates/analysis/CFGAnalyzer.h"
#include "predicates/analysis/InputValidator.h"
#include "predicates/export/CompletePathConditionBuilder.h"
#include "predicates/utils/ConditionExtractor.h"

#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace {
struct AnalyzedExample {
  PathNodeSet paths;
  std::map<std::string, std::string> branchLabels;
};

AnalyzedExample analyze(const std::string &code, unsigned passes = 3) {
  AnalyzedExample result;
  auto ast = clang::tooling::buildASTFromCode(code, "incremental.c");
  if (!ast) {
    ADD_FAILURE() << "Could not parse regression input";
    return result;
  }
  for (auto *decl : ast->getASTContext().getTranslationUnitDecl()->decls()) {
    auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!function || !function->hasBody()) {
      continue;
    }
    auto cfg = clang::CFG::buildCFG(function, function->getBody(),
                                    &ast->getASTContext(), {});
    if (!cfg) {
      ADD_FAILURE() << "Could not build regression CFG";
      return result;
    }
    for (auto *block : cfg->const_nodes()) {
      if (!block->getTerminatorStmt() || block->succ_empty()) {
        continue;
      }
      const std::string guard =
          PredicateUtils::ConditionExtractor::getTerminatorCondition(
              block, PredicateUtils::BranchType::TrueBranch);
      if (!guard.empty()) {
        result.branchLabels[guard] = "B" + std::to_string(block->getBlockID());
      }
    }
    CFGAnalyzer analyzer(cfg.get(), passes);
    analyzer.analyze(false);
    result.paths = *analyzer.getEntryNodes();
    return result;
  }
  ADD_FAILURE() << "No function in regression input";
  return result;
}

TEST(IncrementalCore, BranchlessFunctionHasCompletedRoot) {
  const auto result = analyze("int f(int a) { a = a + 1; return a; }");
  const auto *root = result.paths.findNode("");
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(root->reachesExit);
  EXPECT_EQ(result.paths.getExitPathLabels(), std::vector<std::string>{""});
  std::string error;
  const auto completed = CompletePathConditionBuilder::build(
      result.paths, result.paths.getExitPathLabels(), error);
  ASSERT_TRUE(completed.has_value()) << error;
  ASSERT_EQ(completed->size(), 1u);
  EXPECT_EQ(completed->front().pathLabel, "");
  EXPECT_EQ(completed->front().predicate, "true");
}

TEST(IncrementalCore, FalsePrefixPreservesItsAssignmentContext) {
  const auto result = analyze(R"(
    int f(int a, int b) {
      if (a > 0) { b = b + 1; } else { b = b - 1; }
      if (b > 2) { return 1; }
      return 0;
    })");
  const std::string outer = result.branchLabels.at("(a > 0)");
  const std::string inner = result.branchLabels.at("(b > 2)");
  const std::string falsePrefix = outer + "F." + inner;
  const auto *trueEdge = result.paths.findNode(falsePrefix + "T");
  const auto *falseEdge = result.paths.findNode(falsePrefix + "F");
  ASSERT_NE(trueEdge, nullptr);
  ASSERT_NE(falseEdge, nullptr);
  EXPECT_EQ(trueEdge->edgePredicate, "((b - 1) > 2)");
  EXPECT_FALSE(falseEdge->hasStoredEdgePredicate());
  EXPECT_TRUE(trueEdge->reachesExit);
  EXPECT_TRUE(falseEdge->reachesExit);
  EXPECT_EQ(result.paths.resolveEdgePredicate(falsePrefix + "F"),
            "not(((b - 1) > 2))");
  EXPECT_EQ(result.paths.resolvePathCondition(falsePrefix + "T"),
            "(not((a > 0))) and (((b - 1) > 2))");
  EXPECT_FALSE(result.paths.findNode("")->reachesExit);
}

TEST(IncrementalCore, SubstitutesEachAssignmentOnceIncludingIdentifierGuards) {
  const auto arithmetic = analyze(R"(
    int f(int a, int b) {
      a = a + 1;
      b = b + 2;
      if (a < b) { return 1; }
      return 0;
    })");
  const std::string label = arithmetic.branchLabels.at("(a < b)") + "T";
  ASSERT_NE(arithmetic.paths.findNode(label), nullptr);
  EXPECT_EQ(arithmetic.paths.findNode(label)->edgePredicate,
            "((a + 1) < (b + 2))");

  const auto identifier = analyze(
      "int f(int a) { a = 0; if (a) { return 1; } return 0; }");
  const std::string guard = identifier.branchLabels.at("((a) != 0)") + "T";
  ASSERT_NE(identifier.paths.findNode(guard), nullptr);
  EXPECT_EQ(identifier.paths.findNode(guard)->edgePredicate, "(((0)) != 0)");
}

TEST(IncrementalCore, BreakPathIsCompletedDespiteLastLoopOutcomeTrue) {
  const auto result = analyze(R"(
    int f(int a) {
      while (a < 3) {
        if (a == 1) { break; }
        a = a + 1;
      }
      return a;
    })");
  const std::string loop = result.branchLabels.at("(a < 3)");
  const std::string decision = result.branchLabels.at("(a == 1)");
  const std::string breakPath = loop + "T." + decision + "T";
  const auto *node = result.paths.findNode(breakPath);
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(node->reachesExit);
  const auto complete = result.paths.getExitPathLabels();
  EXPECT_NE(std::find(complete.begin(), complete.end(), breakPath), complete.end());
  EXPECT_FALSE(result.paths.findNode(loop + "T")->reachesExit);
}

TEST(IncrementalCore, ContinuePreservesTheLoopBackEdgeWithoutAddingALabel) {
  const auto result = analyze(R"(
    int f(int n) {
      while (n > 0) {
        n = n - 1;
        if (n == 1) { continue; }
        if (n == 0) { break; }
      }
      return n;
    })", 4);
  const std::string loop = result.branchLabels.at("(n > 0)");
  const std::string continuation = result.branchLabels.at("(n == 1)");
  const std::string exitDecision = result.branchLabels.at("(n == 0)");
  // n=2: one continue, then a second iteration ending in break.
  const std::string path = loop + "T." + continuation + "T." + loop +
                           "T." + continuation + "F." + exitDecision + "T";
  const auto *node = result.paths.findNode(path);
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(node->reachesExit);
  EXPECT_TRUE(result.paths.resolvePathCondition(path).has_value());
  const auto labels = result.paths.getExitPathLabels();
  EXPECT_NE(std::find(labels.begin(), labels.end(), path), labels.end());
}

TEST(IncrementalCore, NestedLoopPrefixesRemainResolvableAndExitIsExplicit) {
  const auto result = analyze(R"(
    int f(int a, int b) {
      while (a > 0) {
        b = 0;
        while (b < 2) { b = b + 1; }
        a = a - 1;
      }
      return a;
    })", 5);
  const std::string outer = result.branchLabels.at("(a > 0)");
  const std::string inner = result.branchLabels.at("(b < 2)");
  const std::string oneOuter = outer + "T." + inner + "T." + inner +
                               "T." + inner + "F." + outer + "F";
  const auto *exit = result.paths.findNode(oneOuter);
  ASSERT_NE(exit, nullptr);
  EXPECT_TRUE(exit->reachesExit);
  EXPECT_TRUE(result.paths.resolvePathCondition(oneOuter).has_value());
  const auto firstInner = result.paths.resolveEdgePredicate(outer + "T." + inner + "T");
  ASSERT_TRUE(firstInner.has_value());
  EXPECT_EQ(*firstInner, "((0) < 2)");
  for (const auto &[label, node] : result.paths) {
    EXPECT_TRUE(result.paths.resolvePathCondition(label).has_value()) << label;
    if (!label.empty() && label.back() == 'F') {
      EXPECT_FALSE(node.hasStoredEdgePredicate()) << label;
    }
  }
}

TEST(IncrementalCore, PrunedTrueSuccessorStillDefinesFalseSiblingGuard) {
  const auto result = analyze("int f(int a) { if (0) { return a; } return 0; }");
  const std::string branch = result.branchLabels.at("((0) != 0)");
  const auto falseGuard = result.paths.resolveEdgePredicate(branch + "F");
  ASSERT_TRUE(falseGuard.has_value());
  EXPECT_EQ(*falseGuard, "not(((0) != 0))");
  EXPECT_TRUE(result.paths.findNode(branch + "F")->reachesExit);
  EXPECT_FALSE(result.paths.findNode(branch + "T")->reachesExit);
}

void expectCompletePaths(const AnalyzedExample &result,
                         std::vector<std::string> expected) {
  auto actual = result.paths.getExitPathLabels();
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected);
  for (const auto &label : expected) {
    const auto *node = result.paths.findNode(label);
    ASSERT_NE(node, nullptr) << label;
    EXPECT_TRUE(node->reachesExit) << label;
  }
}

TEST(IncrementalCore, LogicalOrUsesLocalConditionsAndSkipsRightOperand) {
  const auto result = analyze(R"(
    int f(int x, int y) {
      if (x > 0 || y > 5) { return 1; }
      return 0;
    })");
  const auto a = result.branchLabels.at("(x > 0)");
  const auto b = result.branchLabels.at("(y > 5)");
  expectCompletePaths(result, {a + "T", a + "F." + b + "T",
                               a + "F." + b + "F"});
  EXPECT_FALSE(result.paths.containsNode(a + "T." + b + "T"));
  EXPECT_EQ(result.paths.resolveEdgePredicate(a + "F." + b + "T"),
            "(y > 5)");
  EXPECT_EQ(result.paths.resolvePathCondition(a + "F." + b + "T"),
            "(not((x > 0))) and ((y > 5))");
  EXPECT_EQ(result.paths.resolvePathCondition(a + "F." + b + "F"),
            "(not((x > 0))) and (not((y > 5)))");
}

TEST(IncrementalCore, LogicalAndUsesLocalConditionsAndSkipsRightOperand) {
  const auto result = analyze(R"(
    int f(int x, int y) {
      if (x > 0 && y > 5) { return 1; }
      return 0;
    })");
  const auto a = result.branchLabels.at("(x > 0)");
  const auto b = result.branchLabels.at("(y > 5)");
  expectCompletePaths(result, {a + "F", a + "T." + b + "T",
                               a + "T." + b + "F"});
  EXPECT_FALSE(result.paths.containsNode(a + "F." + b + "T"));
  EXPECT_EQ(result.paths.resolveEdgePredicate(a + "T." + b + "T"),
            "(y > 5)");
  EXPECT_EQ(result.paths.resolvePathCondition(a + "T." + b + "F"),
            "((x > 0)) and (not((y > 5)))");
}

TEST(IncrementalCore, NestedLogicalGuardsPreserveOperandOrder) {
  const auto rightNested = analyze(R"(
    int f(int x, int y, int z) {
      if (x > 0 || (y > 5 && z < 7)) { return 1; }
      return 0;
    })");
  auto a = rightNested.branchLabels.at("(x > 0)");
  auto b = rightNested.branchLabels.at("(y > 5)");
  auto c = rightNested.branchLabels.at("(z < 7)");
  expectCompletePaths(rightNested,
                      {a + "T", a + "F." + b + "F",
                       a + "F." + b + "T." + c + "T",
                       a + "F." + b + "T." + c + "F"});
  EXPECT_EQ(rightNested.paths.resolveEdgePredicate(
                a + "F." + b + "T." + c + "T"), "(z < 7)");

  const auto leftNested = analyze(R"(
    int f(int x, int y, int z) {
      if ((x > 0 || y > 5) && z < 7) { return 1; }
      return 0;
    })");
  a = leftNested.branchLabels.at("(x > 0)");
  b = leftNested.branchLabels.at("(y > 5)");
  c = leftNested.branchLabels.at("(z < 7)");
  expectCompletePaths(leftNested,
                      {a + "T." + c + "T", a + "T." + c + "F",
                       a + "F." + b + "F",
                       a + "F." + b + "T." + c + "T",
                       a + "F." + b + "T." + c + "F"});
}

TEST(IncrementalCore, LogicalGuardsSubstitutePrecedingAssignmentsOnce) {
  const auto result = analyze(R"(
    int f(int x, int y) {
      x = x + 1;
      y = y - 2;
      if (x > 0 || y > x) { return 1; }
      return 0;
    })");
  const auto a = result.branchLabels.at("(x > 0)");
  const auto b = result.branchLabels.at("(y > x)");
  expectCompletePaths(result, {a + "T", a + "F." + b + "T",
                               a + "F." + b + "F"});
  EXPECT_EQ(result.paths.resolveEdgePredicate(a + "T"), "((x + 1) > 0)");
  EXPECT_EQ(result.paths.resolveEdgePredicate(a + "F." + b + "T"),
            "((y - 2) > (x + 1))");
}

TEST(IncrementalCore, ScalarLogicalOperandsNormalizeToNonzero) {
  const auto result = analyze(R"(
    int f(int x, int y) {
      if (x && y) { return 1; }
      return 0;
    })");
  const auto a = result.branchLabels.at("((x) != 0)");
  const auto b = result.branchLabels.at("((y) != 0)");
  expectCompletePaths(result, {a + "F", a + "T." + b + "T",
                               a + "T." + b + "F"});
  EXPECT_EQ(result.paths.resolvePathCondition(a + "T." + b + "F"),
            "(((x) != 0)) and (not(((y) != 0)))");
}

TEST(IncrementalCore, ConstantLogicalGuardsKeepPrunedSuccessorPolarity) {
  const auto zeroAnd = analyze(R"(
    int f(int y) {
      if (0 && y > 5) { return 1; }
      return 0;
    })");
  const auto zero = zeroAnd.branchLabels.at("((0) != 0)");
  expectCompletePaths(zeroAnd, {zero + "F"});
  EXPECT_EQ(zeroAnd.paths.resolveEdgePredicate(zero + "F"),
            "not(((0) != 0))");
  EXPECT_FALSE(zeroAnd.paths.findNode(zero + "T")->reachesExit);

  const auto oneOr = analyze(R"(
    int f(int y) {
      if (1 || y > 5) { return 1; }
      return 0;
    })");
  const auto one = oneOr.branchLabels.at("((1) != 0)");
  expectCompletePaths(oneOr, {one + "T"});
  EXPECT_EQ(oneOr.paths.resolveEdgePredicate(one + "T"), "((1) != 0)");
}

bool validateSource(const std::string &code, std::string &error) {
  auto ast = clang::tooling::buildASTFromCode(code, "guard-validation.c");
  if (!ast) {
    ADD_FAILURE() << "Could not parse validation regression input";
    return false;
  }
  for (auto *decl : ast->getASTContext().getTranslationUnitDecl()->decls()) {
    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (function && function->hasBody()) {
      return InputValidator::validate(*function, ast->getASTContext(), error);
    }
  }
  ADD_FAILURE() << "Validation input has no function";
  return false;
}

TEST(InputValidation, LogicalTreesAreAcceptedInIfWhileAndForGuards) {
  std::string error;
  EXPECT_TRUE(validateSource(R"(
    int f(int x, int y) {
      if ((x > 0 || y > 5) && x < 10) { return 1; }
      while (x > 0 && y > 0) { x = x - 1; }
      for (x = 0; x < 3 || y < 0; x = x + 1) { return x; }
      return 0;
    })", error)) << error;
}

TEST(InputValidation, LogicalValueExpressionsRemainUnsupported) {
  for (const std::string &code : {
           "int f(int x, int y) { x = x > 0 || y > 5; return x; }",
           "int f(int x, int y) { return x > 0 && y > 5; }",
           "int f(int x, int y) { if ((x && y) + 1) return 1; return 0; }",
           "int f(int x, int y) { if ((x || y) == 0) return 1; return 0; }"}) {
    SCOPED_TRACE(code);
    std::string error;
    EXPECT_FALSE(validateSource(code, error));
    EXPECT_FALSE(error.empty());
  }
}

TEST(InputValidation, LogicalGuardsStillRejectSideEffectsAndBitwiseOperators) {
  for (const std::string &code : {
           "int f(int x, int y) { if (x++ > 0 || y > 5) return 1; return 0; }",
           "int f(int x, int y) { if (x > 0 && (y = 1)) return 1; return 0; }",
           "int f(int x, int y) { if (x & y) return 1; return 0; }"}) {
    SCOPED_TRACE(code);
    std::string error;
    EXPECT_FALSE(validateSource(code, error));
    EXPECT_FALSE(error.empty());
  }
}

TEST(IncrementalCore, LogicalEdgesStayDistinctWhenBothSuccessorsAreTheSame) {
  for (const std::string &operation : {"||", "&&"}) {
    SCOPED_TRACE(operation);
    const auto result = analyze(
        "int f(int x, int y) { if (x > 0 " + operation +
        " y > 5) {} return 0; }");
    const auto a = result.branchLabels.at("(x > 0)");
    const auto b = result.branchLabels.at("(y > 5)");
    const auto prefix = a + (operation == "||" ? "F." : "T.");
    expectCompletePaths(result,
                        {a + (operation == "||" ? "T" : "F"),
                         prefix + b + "T", prefix + b + "F"});
    EXPECT_EQ(result.paths.resolveEdgePredicate(prefix + b + "F"),
              "not((y > 5))");
  }
}
} // namespace

TEST(PathNodeModel, FalseEdgeHasResolvedButNoStoredPredicate) {
  PathNodeSet nodes;
  nodes.getOrCreateNode("B6T").edgePredicate = "((a + 1) < b)";
  nodes.getOrCreateNode("B6F");
  nodes.getOrCreateNode("B6F.B2T").edgePredicate = "(c < (a + 1))";
  nodes.getOrCreateNode("B6F.B2F").reachesExit = true;

  const auto *falseNode = nodes.findNode("B6F.B2F");
  ASSERT_NE(falseNode, nullptr);
  EXPECT_FALSE(falseNode->hasStoredEdgePredicate());
  EXPECT_EQ(nodes.resolveEdgePredicate("B6F.B2F"), "not((c < (a + 1)))");
  EXPECT_EQ(nodes.resolvePathCondition("B6F.B2F"),
            "(not(((a + 1) < b))) and (not((c < (a + 1))))");
  EXPECT_EQ(nodes.getExitPathLabels(), std::vector<std::string>{"B6F.B2F"});
  EXPECT_EQ(nodes.resolvePathCondition(""), "true");
  EXPECT_EQ(nodes.findNode("B99T"), nullptr);
  EXPECT_FALSE(nodes.containsNode("B99T"));
}

TEST(PathNodeModel, SharedLabelRulesPreserveEarlierDecisions) {
  for (const std::string label : {"", "B0T", "B6F.B2T", "B3T.B3T.B3F"})
    EXPECT_TRUE(PathLabels::isValid(label)) << label;
  for (const std::string label : {"B", "B1", "B1X", ".B1T", "B1T.", "B1T..B2F"})
    EXPECT_FALSE(PathLabels::isValid(label)) << label;
  EXPECT_FALSE(PathLabels::isTrueEdge(""));
  EXPECT_FALSE(PathLabels::isFalseEdge(""));
  EXPECT_EQ(PathLabels::parentLabel("B6F.B2F"), "B6F");
  EXPECT_EQ(PathLabels::parentLabel("B6F"), "");
  EXPECT_EQ(PathLabels::trueSiblingLabel("B6F.B2F"), "B6F.B2T");
  EXPECT_EQ(PathLabels::prepend("B6F", "B2F"), "B6F.B2F");
  EXPECT_EQ(PathLabels::prepend("B6F", ""), "B6F");
  EXPECT_EQ(PathLabels::prepend("", "B2F"), "B2F");
}

TEST(PathConditionBuilder, SelectionIsOrderedAndResolutionFailureIsExplicit) {
  PathNodeSet nodes;
  nodes.getOrCreateNode("B1T").edgePredicate = "(x > 0)";
  nodes.getOrCreateNode("B1F").reachesExit = true;
  std::string error = "old error";
  const auto selected = CompletePathConditionBuilder::build(
      nodes, {"B1F", "", "B1T", "B1F"}, error);
  ASSERT_TRUE(selected.has_value()) << error;
  EXPECT_TRUE(error.empty());
  ASSERT_EQ(selected->size(), 3u);
  EXPECT_EQ((*selected)[0].pathLabel, "B1F");
  EXPECT_EQ((*selected)[0].predicate, "(not((x > 0)))");
  EXPECT_EQ((*selected)[1].pathLabel, "");
  EXPECT_EQ((*selected)[1].predicate, "true");
  EXPECT_EQ((*selected)[2].pathLabel, "B1T");

  // A valid first objective must not hide a later unresolved one.
  nodes.getOrCreateNode("B2F"); // Missing corresponding true predicate.
  const auto invalid = CompletePathConditionBuilder::build(nodes, {"B1T", "B2F"}, error);
  EXPECT_FALSE(invalid.has_value());
  EXPECT_EQ(error, "Cannot resolve target path 'B2F'");
  const auto empty = CompletePathConditionBuilder::build(nodes, {}, error);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  EXPECT_TRUE(error.empty());
}
