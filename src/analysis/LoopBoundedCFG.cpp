#include "LoopCounterStack.h"
#include "predicates/analysis/CFGAnalyzer.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <utility>
#include <vector>

using namespace clang;

namespace {
using bounded_loops::CounterStack;
using bounded_loops::LoopCounters;

using Delta = std::map<CounterStack, PredicateAnalyzer>;

struct BlockState {
  // An identical label in two active loop contexts must propagate separately.
  std::map<CounterStack, std::map<std::string, PathNode>> records;
  Delta delta;
};

struct PassChanges {
  size_t added = 0;
  size_t updated = 0;
  size_t exits = 0;

  bool changed() const { return added != 0 || updated != 0; }
};

void printStack(const CounterStack &stack) {
  llvm::outs() << " [";
  bool first = true;
  for (const auto &[loop, count] : stack) {
    if (!first)
      llvm::outs() << ", ";
    llvm::outs() << "B" << loop << "=" << count;
    first = false;
  }
  llvm::outs() << "]";
}

void printCutExits(const CFGBlock &block, const PredicateAnalyzer &suffixes,
                   const CounterStack &stack, unsigned bound) {
  for (const auto &[label, node] : suffixes.getNodes()) {
    if (!node.reachesExit)
      continue;
    llvm::outs() << "  B" << block.getBlockID() << " cut B"
                 << block.getBlockID() << "T";
    if (!label.empty())
      llvm::outs() << "." << label;
    printStack(stack);
    llvm::outs() << " (body-entry bound " << bound << ")\n";
  }
}

// Each (block, counter stack, label) can acquire a predicate and exit
// reachability. A repeated transfer of the same facts adds no work.
bool collect(BlockState &state, Delta &next, const CounterStack &stack,
             const PredicateAnalyzer &incoming, PassChanges &changes,
             const CFGBlock &block, bool verbose) {
  auto &records = state.records[stack];
  for (const auto &[label, node] : incoming.getNodes()) {
    auto [pos, added] = records.try_emplace(label);
    auto &record = pos->second;
    if (record.hasStoredEdgePredicate() && node.hasStoredEdgePredicate() &&
        record.edgePredicate != node.edgePredicate) {
      llvm::errs() << "Conflicting edge predicates for " << label << " at B"
                   << block.getBlockID() << "\n";
      return false;
    }
    const bool newPredicate =
        !record.hasStoredEdgePredicate() && node.hasStoredEdgePredicate();
    const bool newExit = !record.reachesExit && node.reachesExit;
    if (!added && !newPredicate && !newExit)
      continue;

    if (newPredicate)
      record.edgePredicate = node.edgePredicate;
    record.reachesExit |= node.reachesExit;
    next[stack].getNodes().getOrCreateNode(label) = record;
    if (added)
      ++changes.added;
    else
      ++changes.updated;
    if (newExit)
      ++changes.exits;
    if (verbose) {
      llvm::outs() << "  B" << block.getBlockID() << " "
                   << (added ? "add " : "update ")
                   << (label.empty() ? "<root>" : label);
      printStack(stack);
      if (record.reachesExit)
        llvm::outs() << " exit";
      if (record.hasStoredEdgePredicate())
        llvm::outs() << "  " << record.edgePredicate;
      llvm::outs() << "\n";
    }
  }
  return true;
}
} // namespace

bool CFGAnalyzer::analyzeWithLoopBound(unsigned maxBodyEntries, bool verbose) {
  loopBound_ = maxBodyEntries;
  analysisPasses_ = 0;
  blockPredicateAnalyzers_.clear();
  const LoopCounters counters(*cfg_);
  std::vector<CFGBlock *> blocks(cfg_->getNumBlockIDs(), nullptr);
  for (auto *block : cfg_->const_nodes())
    blocks[block->getBlockID()] = block;
  std::vector<BlockState> states(blocks.size());
  PredicateAnalyzer exitSeed;
  exitSeed.getNodes().getOrCreateNode("").reachesExit = true;
  const PredicateAnalyzer openSeed;

  bool changed;
  do {
    ++analysisPasses_;
    changed = false;
    if (verbose)
      llvm::outs() << "\n=== Loop-bound pass " << analysisPasses_ << " ===\n";

    for (auto *block : blocks) {
      if (!block)
        continue;
      auto &state = states[block->getBlockID()];
      Delta next;
      PassChanges changes;
      if (analysisPasses_ == 1 && block == &cfg_->getExit() &&
          !collect(state, next, {}, exitSeed, changes, *block, verbose))
        return false;

      unsigned slot = 0;
      for (const auto &edge : block->succs()) {
        const unsigned edgeSlot = slot++;
        auto *successor = edge.getReachableBlock();
        if (!successor)
          continue;

        // Keep the guard of a first over-bound body entry as an open prefix.
        // Only the real function exit seeds a completed path.
        if (analysisPasses_ == 1 &&
            counters.entersBody(*block, *successor, edgeSlot)) {
          PredicateAnalyzer guard;
          guard.propagateFromSuccessor(block, successor, openSeed, edgeSlot);
          if (!collect(state, next, {}, guard, changes, *block, verbose))
            return false;
        }

        for (const auto &[stack, suffixes] :
             states[successor->getBlockID()].delta) {
          if (verbose) {
            llvm::outs() << "  B" << block->getBlockID() << " <- B"
                         << successor->getBlockID();
            printStack(stack);
            llvm::outs() << "\n";
          }
          auto prior = counters.prepend(*block, *successor, edgeSlot, stack,
                                        maxBodyEntries);
          if (!prior) {
            if (verbose)
              printCutExits(*block, suffixes, stack, maxBodyEntries);
            continue;
          }
          PredicateAnalyzer transferred;
          transferred.propagateFromSuccessor(block, successor, suffixes,
                                             edgeSlot);
          if (!collect(state, next, *prior, transferred, changes, *block,
                       verbose))
            return false;
        }
      }
      // Lower-numbered successors have already published this pass; the others
      // still expose the preceding pass. A self edge also reads the old delta.
      state.delta = std::move(next);
      changed |= changes.changed();
      if (verbose)
        llvm::outs() << "Block B" << block->getBlockID()
                     << " | new records: " << changes.added
                     << " | updated records: " << changes.updated
                     << " | new exits: " << changes.exits << "\n";
    }
    if (verbose)
      llvm::outs() << "Pass " << analysisPasses_
                   << ": changed=" << (changed ? "true" : "false") << "\n";
  } while (changed);

  // Counters belong to traversal. Export one set of incremental path records
  // per block, with the same label/predicate and completion semantics as PClib.
  for (auto *block : blocks) {
    if (!block)
      continue;
    auto &nodes = blockPredicateAnalyzers_[block].getNodes();
    for (const auto &context : states[block->getBlockID()].records) {
      for (const auto &[label, record] : context.second) {
        auto &target = nodes.getOrCreateNode(label);
        if (record.hasStoredEdgePredicate())
          target.edgePredicate = record.edgePredicate;
        target.reachesExit |= record.reachesExit;
      }
    }
  }
  if (verbose)
    printBlocks();
  return true;
}
