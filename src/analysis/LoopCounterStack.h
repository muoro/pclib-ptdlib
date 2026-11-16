#ifndef PCCSCC_LOOP_COUNTER_STACK_H
#define PCCSCC_LOOP_COUNTER_STACK_H

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bounded_loops {

// From outer to inner: (loop-header block ID, body entries in this suffix).
// Exit and open-guard seeds start empty. An activation first met on a
// backward path through break or return has no entries counted yet.
using CounterStack = std::vector<std::pair<unsigned, unsigned>>;

class LoopCounters {
public:
  explicit LoopCounters(const clang::CFG &cfg) {
    for (const clang::CFGBlock *block : cfg.const_nodes()) {
      const clang::Stmt *terminator = block->getTerminatorStmt();
      const clang::Expr *condition;
      if (const auto *loop =
              llvm::dyn_cast_or_null<clang::WhileStmt>(terminator))
        condition = loop->getCond();
      else if (const auto *loop =
                   llvm::dyn_cast_or_null<clang::ForStmt>(terminator))
        condition = loop->getCond();
      else
        continue;

      headers_.emplace(terminator, block);
      registerLogicalGuards(condition, block);
    }
  }

  bool entersBody(const clang::CFGBlock &from, const clang::CFGBlock &to,
                  unsigned slot) const {
    const auto *loop = header(from);
    return loop && slot == 0 && loop != header(to);
  }

  // Prepend the forward edge from -> to to a suffix already at 'to'.
  std::optional<CounterStack> prepend(const clang::CFGBlock &from,
                                      const clang::CFGBlock &to, unsigned slot,
                                      const CounterStack &suffix,
                                      unsigned bound) const {
    const auto *source = header(from);
    const auto *target = header(to);
    CounterStack stack = suffix;
    if (source == target)
      return stack; // Ordinary edge, or another operand of the same guard.

    // Crossing the start of the target activation backwards relinquishes its
    // counter. Its own latch returns to the guard within the same activation.
    if (target && from.getLoopTarget() != target->getTerminatorStmt() &&
        !stack.empty() && stack.back().first == target->getBlockID())
      stack.pop_back();

    if (source) {
      const unsigned id = source->getBlockID();
      if (stack.empty() || stack.back().first != id)
        stack.emplace_back(id, 0);
      unsigned &entries = stack.back().second;
      if (slot == 0) {
        if (entries == bound)
          return std::nullopt;
        ++entries;
      } else {
        entries = 0; // Normal exit: no body entry beyond this guard.
      }
    }
    return stack;
  }

private:
  const clang::CFGBlock *header(const clang::CFGBlock &block) const {
    const auto it = headers_.find(block.getTerminatorStmt());
    return it == headers_.end() ? nullptr : it->second;
  }

  void registerLogicalGuards(const clang::Expr *expr,
                             const clang::CFGBlock *headerBlock) {
    if (!expr)
      return;
    const auto *op =
        llvm::dyn_cast<clang::BinaryOperator>(expr->IgnoreParenImpCasts());
    if (op && op->isLogicalOp()) {
      headers_.emplace(op, headerBlock);
      registerLogicalGuards(op->getLHS(), headerBlock);
      registerLogicalGuards(op->getRHS(), headerBlock);
    }
  }

  std::map<const clang::Stmt *, const clang::CFGBlock *> headers_;
};

} // namespace bounded_loops

#endif
