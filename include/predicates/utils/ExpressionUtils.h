#ifndef EXPRESSION_UTILS_H
#define EXPRESSION_UTILS_H

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include <string>

namespace PredicateUtils {

class ExpressionUtils {
public:
    // Convert expression to string
    static std::string exprToString(const clang::Expr *E);

    // Check if expression is complex (contains operators)
    static bool isComplexExpression(const std::string &expr);

    // Extract assignment operands
    static bool extractAssignmentOperands(const clang::Stmt *stmt,
                                        std::string &leftSide,
                                        std::string &rightSide);

    // Substitute variable in expression
    static void substituteVariable(std::string &expression,
                                 const std::string &variable,
                                 const std::string &replacement);
};

} // namespace PredicateUtils

#endif
