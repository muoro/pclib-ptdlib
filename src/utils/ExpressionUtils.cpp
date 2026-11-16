#include "predicates/utils/ExpressionUtils.h"
#include "clang/AST/Expr.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"
#include <regex>

using namespace clang;
using namespace PredicateUtils;

std::string ExpressionUtils::exprToString(const Expr *E) {
    if (!E) return "";

    std::string str;
    llvm::raw_string_ostream stream(str);
    E->printPretty(stream, nullptr, PrintingPolicy(clang::LangOptions()));
    return str;
}

bool ExpressionUtils::isComplexExpression(const std::string &expr) {
    return (expr.find('<') != std::string::npos ||
            expr.find('>') != std::string::npos ||
            expr.find("==") != std::string::npos ||
            expr.find("!=") != std::string::npos ||
            expr.find('+') != std::string::npos ||
            expr.find('-') != std::string::npos ||
            expr.find('*') != std::string::npos ||
            expr.find('/') != std::string::npos);
}

bool ExpressionUtils::extractAssignmentOperands(const Stmt *stmt,
                                               std::string &leftSide,
                                               std::string &rightSide) {
    if (const BinaryOperator *binOp = llvm::dyn_cast<BinaryOperator>(stmt)) {
        if (binOp->isAssignmentOp()) {
            const Expr *lhs = binOp->getLHS();
            const Expr *rhs = binOp->getRHS();

            leftSide = exprToString(lhs);
            rightSide = exprToString(rhs);
            return true;
        }
    }
    return false;
}

void ExpressionUtils::substituteVariable(std::string &expression,
                                        const std::string &variable,
                                        const std::string &replacement) {
    std::regex varRegex("\\b" + variable + "\\b");
    expression = std::regex_replace(expression, varRegex, replacement);

}
