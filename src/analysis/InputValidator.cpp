#include "predicates/analysis/InputValidator.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"

#include <set>
#include <string>

namespace {

class Validator {
public:
  Validator(clang::ASTContext &context, std::string &error)
      : context_(context), error_(error) {}

  bool validate(const clang::FunctionDecl &function) {
    if (!function.hasBody() || function.isVariadic()) {
      return fail("requires a defined, non-variadic function");
    }
    if (!function.getReturnType()->isVoidType() &&
        !isSigned32(function.getReturnType())) {
      return fail("return type must be void or a signed 32-bit integer");
    }
    for (const auto *parameter : function.parameters()) {
      const std::string name = parameter->getNameAsString();
      if (name.empty() || !isSigned32(parameter->getType()) ||
          !names_.insert(name).second || name == "true" || name == "false" ||
          name == "and" || name == "not") {
        return fail(
            "parameters must be named, non-volatile signed 32-bit integers");
      }
      variables_.insert(parameter);
    }
    return statement(function.getBody());
  }

private:
  bool fail(const std::string &reason) {
    error_ = reason;
    return false;
  }

  bool isSigned32(clang::QualType type) const {
    return !type.isVolatileQualified() && !type->isAtomicType() &&
           !type->isEnumeralType() && type->isSignedIntegerType() &&
           context_.getTypeSize(type) == 32;
  }

  bool guard(const clang::Expr *expr) {
    if (!expr || expr->HasSideEffects(context_)) {
      return fail(
          "guards must be explicit, side-effect-free scalar expressions");
    }
    return guardExpression(expr);
  }

  bool guardExpression(const clang::Expr *expr) {
    if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
      return guardExpression(paren->getSubExpr());
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      const auto kind = cast->getCastKind();
      if (kind == clang::CK_LValueToRValue || kind == clang::CK_NoOp ||
          kind == clang::CK_IntegralToBoolean) {
        return guardExpression(cast->getSubExpr());
      }
      return expression(expr);
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
      if (binary->isLogicalOp()) {
        // Clang splits these guard trees into atomic CFG decisions. Logical
        // values in assignments/returns still use expression() and are rejected.
        return guardExpression(binary->getLHS()) &&
               guardExpression(binary->getRHS());
      }
    }
    return expression(expr);
  }

  bool expression(const clang::Expr *expr) {
    if (!expr) {
      return fail("missing expression");
    }
    if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
      return expression(paren->getSubExpr());
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      const auto kind = cast->getCastKind();
      if (kind != clang::CK_LValueToRValue && kind != clang::CK_NoOp &&
          kind != clang::CK_IntegralToBoolean) {
        return fail("implicit type conversions outside signed 32-bit "
                    "expressions are unsupported");
      }
      return expression(cast->getSubExpr());
    }
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
      const auto *variable =
          llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
      return variable && variables_.count(variable) &&
                     isSigned32(expr->getType())
                 ? true
                 : fail("only scalar parameters and previously initialized "
                        "local variables may be read or assigned");
    }
    if (const auto *literal = llvm::dyn_cast<clang::IntegerLiteral>(expr)) {
      if (!isSigned32(literal->getType()) ||
          literal->getValue().getActiveBits() > 31) {
        return fail(
            "integer literals must fit the signed 32-bit expression model");
      }
      return true;
    }
    if (llvm::isa<clang::CXXBoolLiteralExpr>(expr)) {
      return true;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
      if ((unary->getOpcode() != clang::UO_Plus &&
           unary->getOpcode() != clang::UO_Minus) ||
          !isSigned32(unary->getType())) {
        return fail("only unary + and - on signed 32-bit values are supported");
      }
      return expression(unary->getSubExpr());
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
      switch (binary->getOpcode()) {
      case clang::BO_Add:
      case clang::BO_Sub:
      case clang::BO_Mul:
        if (!isSigned32(binary->getType())) {
          return fail("arithmetic requires signed 32-bit operands");
        }
        break;
      case clang::BO_LT:
      case clang::BO_GT:
      case clang::BO_LE:
      case clang::BO_GE:
      case clang::BO_EQ:
      case clang::BO_NE:
        break;
      default:
        return fail("unsupported expression operator '" +
                    binary->getOpcodeStr().str() + "'");
      }
      return expression(binary->getLHS()) && expression(binary->getRHS());
    }
    return fail("unsupported expression: " +
                std::string(expr->getStmtClassName()));
  }

  bool statement(const clang::Stmt *stmt) {
    if (!stmt) {
      return true;
    }
    if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
      for (const auto *child : compound->body()) {
        if (!statement(child)) {
          return false;
        }
      }
      return true;
    }
    if (const auto *declarations = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const auto *declaration : declarations->decls()) {
        const auto *variable = llvm::dyn_cast<clang::VarDecl>(declaration);
        if (!variable || !variable->isLocalVarDecl() ||
            !variable->hasLocalStorage() ||
            variable->getTLSKind() != clang::VarDecl::TLS_None ||
            !isSigned32(variable->getType())) {
          return fail("local declarations must be automatic, non-volatile "
                      "signed 32-bit integers");
        }
        const std::string name = variable->getNameAsString();
        if (name.empty() || name == "true" || name == "false" ||
            name == "and" || name == "not") {
          return fail("local variables must have supported scalar names");
        }
        // Predicates currently identify variables by name. Reject shadowing
        // (including reused names in disjoint scopes) instead of conflating
        // different VarDecl objects during textual substitution.
        if (names_.count(name)) {
          return fail("local variable names must be unique across the "
                      "function; shadowing is unsupported");
        }
        if (!variable->hasInit()) {
          return fail("local variables require an initializer");
        }
        // Register only after checking the initializer: int x = x is not an
        // independent symbolic input and must not introduce an unbound value.
        if (!expression(variable->getInit())) {
          return false;
        }
        variables_.insert(variable);
        names_.insert(name);
      }
      return true;
    }
    if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      if (branch->getInit() || branch->getConditionVariableDeclStmt()) {
        return fail("declarations or initializers inside if statements are "
                    "unsupported");
      }
      return guard(branch->getCond()) && statement(branch->getThen()) &&
             statement(branch->getElse());
    }
    if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(stmt)) {
      if (loop->getConditionVariableDeclStmt()) {
        return fail("declarations inside while conditions are unsupported");
      }
      return guard(loop->getCond()) && statement(loop->getBody());
    }
    if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(stmt)) {
      if (loop->getConditionVariableDeclStmt()) {
        return fail("declarations inside for conditions are unsupported");
      }
      return statement(loop->getInit()) && guard(loop->getCond()) &&
             statement(loop->getInc()) && statement(loop->getBody());
    }
    if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
      return !ret->getRetValue() || expression(ret->getRetValue());
    }
    if (llvm::isa<clang::NullStmt, clang::BreakStmt, clang::ContinueStmt>(
            stmt)) {
      return true;
    }
    if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
      const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr);
      if (binary && binary->getOpcode() == clang::BO_Assign) {
        const auto *lhs = llvm::dyn_cast<clang::DeclRefExpr>(binary->getLHS());
        const auto *variable =
            lhs ? llvm::dyn_cast<clang::VarDecl>(lhs->getDecl()) : nullptr;
        if (!variable || !variables_.count(variable) ||
            variable->getType().isConstQualified()) {
          return fail("assignments must target a mutable scalar parameter "
                      "or initialized local variable");
        }
        return expression(binary->getRHS());
      }
      return expression(expr);
    }
    return fail("unsupported statement: " +
                std::string(stmt->getStmtClassName()));
  }

  clang::ASTContext &context_;
  std::string &error_;
  std::set<const clang::VarDecl *> variables_;
  std::set<std::string> names_;
};

} // namespace

bool InputValidator::validate(const clang::FunctionDecl &function,
                              clang::ASTContext &context, std::string &error) {
  error.clear();
  return Validator(context, error).validate(function);
}
