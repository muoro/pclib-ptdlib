#include "PredicateParser.h"

#ifdef HAVE_Z3

#include "predicates/model/PathLabel.h"

#include <cctype>
#include <cstdint>
#include <string>

PredicateParser::PredicateParser(
    const std::string &input, z3::context &context,
    const std::vector<std::string> &allowedInputNames, bool wrapArithmetic)
    : input_(input), context_(context), wrapArithmetic_(wrapArithmetic),
      allowedInputNames_(allowedInputNames.begin(), allowedInputNames.end()) {
  nextToken();
}

z3::expr PredicateParser::parse(std::string &error) {
  auto expression = parseBoolExpression(error);
  if (!error.empty())
    return z3::expr(context_);
  if (current_.kind != TokenKind::End) {
    error = "unexpected token '" + current_.text + "'";
    return z3::expr(context_);
  }
  if (!expression.is_bool()) {
    error = "predicate does not produce a boolean expression";
    return z3::expr(context_);
  }
  return expression;
}

const std::map<std::string, z3::expr> &PredicateParser::symbols() const {
  return symbols_;
}

void PredicateParser::nextToken() {
  while (position_ < input_.size() &&
         std::isspace(static_cast<unsigned char>(input_[position_]))) {
    ++position_;
  }

  if (position_ >= input_.size()) {
    current_ = {TokenKind::End, ""};
    return;
  }

  const char ch = input_[position_];
  if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
    const size_t start = position_;
    while (position_ < input_.size() &&
           (std::isalnum(static_cast<unsigned char>(input_[position_])) ||
            input_[position_] == '_' || input_[position_] == '.')) {
      ++position_;
    }

    std::string text = input_.substr(start, position_ - start);
    if (text == "not") {
      current_ = {TokenKind::Not, text};
    } else if (text == "and") {
      current_ = {TokenKind::And, text};
    } else {
      current_ = {TokenKind::Identifier, text};
    }
    return;
  }

  if (std::isdigit(static_cast<unsigned char>(ch))) {
    const size_t start = position_;
    while (position_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
    current_ = {TokenKind::Number, input_.substr(start, position_ - start)};
    return;
  }

  ++position_;
  switch (ch) {
  case '(':
    current_ = {TokenKind::LParen, "("};
    break;
  case ')':
    current_ = {TokenKind::RParen, ")"};
    break;
  case '+':
    current_ = {TokenKind::Plus, "+"};
    break;
  case '-':
    current_ = {TokenKind::Minus, "-"};
    break;
  case '*':
    current_ = {TokenKind::Star, "*"};
    break;
  case '/':
    current_ = {TokenKind::Slash, "/"};
    break;
  case '<':
    if (position_ < input_.size() && input_[position_] == '=') {
      ++position_;
      current_ = {TokenKind::LessEqual, "<="};
    } else {
      current_ = {TokenKind::Less, "<"};
    }
    break;
  case '>':
    if (position_ < input_.size() && input_[position_] == '=') {
      ++position_;
      current_ = {TokenKind::GreaterEqual, ">="};
    } else {
      current_ = {TokenKind::Greater, ">"};
    }
    break;
  case '=':
    if (position_ < input_.size() && input_[position_] == '=') {
      ++position_;
      current_ = {TokenKind::EqualEqual, "=="};
    } else {
      current_ = {TokenKind::Unknown, "="};
    }
    break;
  case '!':
    if (position_ < input_.size() && input_[position_] == '=') {
      ++position_;
      current_ = {TokenKind::BangEqual, "!="};
    } else {
      current_ = {TokenKind::Unknown, "!"};
    }
    break;
  default:
    current_ = {TokenKind::Unknown, std::string(1, ch)};
    break;
  }
}

z3::expr
PredicateParser::parseBoolExpression(std::string &error) {
  z3::expr lhs = parseUnaryBool(error);
  if (!error.empty()) {
    return z3::expr(context_);
  }

  while (current_.kind == TokenKind::And) {
    nextToken();
    z3::expr rhs = parseUnaryBool(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (!lhs.is_bool() || !rhs.is_bool()) {
      error = "'and' requires boolean operands";
      return z3::expr(context_);
    }
    lhs = lhs && rhs;
  }
  return lhs;
}

z3::expr
PredicateParser::parseUnaryBool(std::string &error) {
  if (current_.kind != TokenKind::Not) {
    return parseComparison(error);
  }

  nextToken();
  z3::expr operand(context_);
  if (current_.kind == TokenKind::LParen) {
    nextToken();
    operand = parseBoolExpression(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (current_.kind != TokenKind::RParen) {
      error = "expected ')' after not expression";
      return z3::expr(context_);
    }
    nextToken();
  } else {
    operand = parseUnaryBool(error);
  }

  if (!error.empty())
    return z3::expr(context_);
  if (!operand.is_bool()) {
    error = "'not' requires a boolean operand";
    return z3::expr(context_);
  }
  return !operand;
}

z3::expr
PredicateParser::parseComparison(std::string &error) {
  z3::expr lhs = parseAddSub(error);
  if (!error.empty()) {
    return z3::expr(context_);
  }

  const TokenKind comparison = current_.kind;
  if (comparison != TokenKind::Less && comparison != TokenKind::LessEqual &&
      comparison != TokenKind::Greater &&
      comparison != TokenKind::GreaterEqual &&
      comparison != TokenKind::EqualEqual &&
      comparison != TokenKind::BangEqual) {
    return lhs;
  }
  if (lhs.is_bool()) {
    error = "left side of comparison is not arithmetic";
    return z3::expr(context_);
  }

  nextToken();
  z3::expr rhs = parseAddSub(error);
  if (!error.empty()) {
    return z3::expr(context_);
  }
  if (rhs.is_bool()) {
    error = "right side of comparison is not arithmetic";
    return z3::expr(context_);
  }

  switch (comparison) {
  case TokenKind::Less:
    return lhs < rhs;
  case TokenKind::LessEqual:
    return lhs <= rhs;
  case TokenKind::Greater:
    return lhs > rhs;
  case TokenKind::GreaterEqual:
    return lhs >= rhs;
  case TokenKind::EqualEqual:
    return lhs == rhs;
  case TokenKind::BangEqual:
    return lhs != rhs;
  default:
    return lhs;
  }
}

z3::expr PredicateParser::parseAddSub(std::string &error) {
  z3::expr lhs = parseMulDiv(error);
  if (!error.empty()) {
    return z3::expr(context_);
  }

  while (current_.kind == TokenKind::Plus || current_.kind == TokenKind::Minus) {
    const TokenKind op = current_.kind;
    nextToken();
    z3::expr rhs = parseMulDiv(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (lhs.is_bool() || rhs.is_bool()) {
      error = "arithmetic operator requires arithmetic operands";
      return z3::expr(context_);
    }
    lhs = op == TokenKind::Plus ? lhs + rhs : lhs - rhs;
  }
  return lhs;
}

z3::expr PredicateParser::parseMulDiv(std::string &error) {
  z3::expr lhs = parseFactor(error);
  if (!error.empty()) {
    return z3::expr(context_);
  }

  while (current_.kind == TokenKind::Star || current_.kind == TokenKind::Slash) {
    const TokenKind op = current_.kind;
    nextToken();
    z3::expr rhs = parseFactor(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (lhs.is_bool() || rhs.is_bool()) {
      error = "arithmetic operator requires arithmetic operands";
      return z3::expr(context_);
    }
    if (op == TokenKind::Slash && !wrapArithmetic_) {
      error = "division is outside the supported mathematical integer subset";
      return z3::expr(context_);
    }
    lhs = op == TokenKind::Star ? lhs * rhs : lhs / rhs;
  }
  return lhs;
}

z3::expr PredicateParser::parseFactor(std::string &error) {
  switch (current_.kind) {
  case TokenKind::Number: {
    const std::string value = current_.text;
    nextToken();
    return wrapArithmetic_ ? context_.bv_val(value.c_str(), 32)
                           : context_.int_val(value.c_str());
  }
  case TokenKind::Identifier: {
    const std::string name = current_.text;
    if (name == "true" || name == "false") {
      nextToken();
      return context_.bool_val(name == "true");
    }
    if (!name.empty() && PathLabels::isValid(name) &&
        !allowedInputNames_.count(name)) {
      error = "unexpanded path label '" + name + "'";
      return z3::expr(context_);
    }
    nextToken();
    auto it = symbols_.find(name);
    if (it == symbols_.end()) {
      const auto symbol = wrapArithmetic_ ? context_.bv_const(name.c_str(), 32)
                                          : context_.int_const(name.c_str());
      it = symbols_.emplace(name, symbol).first;
    }
    return it->second;
  }
  case TokenKind::Plus:
  case TokenKind::Minus: {
    const bool negate = current_.kind == TokenKind::Minus;
    nextToken();
    z3::expr operand = parseFactor(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (operand.is_bool()) {
      error = "unary sign requires an arithmetic operand";
      return z3::expr(context_);
    }
    return negate ? -operand : operand;
  }
  case TokenKind::LParen: {
    nextToken();
    z3::expr inner = parseBoolExpression(error);
    if (!error.empty()) {
      return z3::expr(context_);
    }
    if (current_.kind != TokenKind::RParen) {
      error = "expected ')'";
      return z3::expr(context_);
    }
    nextToken();
    return inner;
  }
  case TokenKind::Unknown:
    error = "unsupported token '" + current_.text + "'";
    return z3::expr(context_);
  default:
    error = "unexpected token '" + current_.text + "'";
    return z3::expr(context_);
  }
}

#endif // HAVE_Z3
