#ifndef INPUT_VALIDATOR_H
#define INPUT_VALIDATOR_H

#include <string>

namespace clang {
class ASTContext;
class FunctionDecl;
} // namespace clang

// Rejects source constructs outside the current substitution/solver model.
// Acceptance is a supported-subset check, not a proof for general C semantics.
class InputValidator {
public:
  static bool validate(const clang::FunctionDecl &function,
                       clang::ASTContext &context, std::string &error);
};

#endif
