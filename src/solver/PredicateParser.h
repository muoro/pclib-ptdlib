#ifndef PREDICATE_PARSER_H
#define PREDICATE_PARSER_H

#ifdef HAVE_Z3

#include <z3++.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class PredicateParser {
public:
  PredicateParser(const std::string &input, z3::context &context,
                  const std::vector<std::string> &allowedInputNames = {},
                  bool wrapArithmetic = false);

  z3::expr parse(std::string &error);
  const std::map<std::string, z3::expr> &symbols() const;

private:
  enum class TokenKind : std::uint8_t {
    End,
    Identifier,
    Number,
    LParen,
    RParen,
    Plus,
    Minus,
    Star,
    Slash,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    EqualEqual,
    BangEqual,
    Not,
    And,
    Unknown
  };

  struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
  };

  void nextToken();
  z3::expr parseBoolExpression(std::string &error);
  z3::expr parseUnaryBool(std::string &error);
  z3::expr parseComparison(std::string &error);
  z3::expr parseAddSub(std::string &error);
  z3::expr parseMulDiv(std::string &error);
  z3::expr parseFactor(std::string &error);

  std::string input_;
  size_t position_ = 0;
  Token current_;
  z3::context &context_;
  bool wrapArithmetic_;
  std::map<std::string, z3::expr> symbols_;
  std::set<std::string> allowedInputNames_;
};

#endif // HAVE_Z3

#endif // PREDICATE_PARSER_H
