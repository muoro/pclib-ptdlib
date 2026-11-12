#ifndef PREDICATES_MODEL_PATH_NODE_H
#define PREDICATES_MODEL_PATH_NODE_H

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "predicates/model/PathLabel.h"

// One path-tree node. A nonempty predicate belongs only to the final true edge,
// expressed in the values at the entry of the block owning this analysis state.
// False edges are structural nodes; their predicates negate their true sibling.
// The empty label denotes the root, whose complete path condition is true.
struct PathNode {
  std::string edgePredicate;
  bool reachesExit = false;

  bool hasStoredEdgePredicate() const { return !edgePredicate.empty(); }
};

class PathNodeSet {
private:
  // The map key is the only stored copy of a node's label.
  std::map<std::string, PathNode> nodes_;

public:
  PathNodeSet() { getOrCreateNode(""); }

  PathNode &getOrCreateNode(const std::string &pathLabel) {
    auto result = nodes_.try_emplace(pathLabel);
    return result.first->second;
  }

  bool containsNode(const std::string &pathLabel) const {
    return nodes_.find(pathLabel) != nodes_.end();
  }

  PathNode *findNode(const std::string &pathLabel) {
    auto it = nodes_.find(pathLabel);
    return it == nodes_.end() ? nullptr : &it->second;
  }

  const PathNode *findNode(const std::string &pathLabel) const {
    auto it = nodes_.find(pathLabel);
    return it == nodes_.end() ? nullptr : &it->second;
  }

  // Resolve just the final edge, never the cumulative path constraint.
  std::optional<std::string>
  resolveEdgePredicate(const std::string &label) const {
    const PathNode *path = findNode(label);
    if (!path || !PathLabels::isValid(label)) {
      return std::nullopt;
    }
    if (label.empty()) {
      return "true";
    }
    if (PathLabels::isTrueEdge(label)) {
      if (!path->hasStoredEdgePredicate()) {
        return std::nullopt;
      }
      return path->edgePredicate;
    }
    if (!PathLabels::isFalseEdge(label) || path->hasStoredEdgePredicate()) {
      return std::nullopt;
    }
    const std::string sibling = PathLabels::trueSiblingLabel(label);
    const PathNode *truePath = findNode(sibling);
    if (!truePath || !truePath->hasStoredEdgePredicate()) {
      return std::nullopt;
    }
    return "not(" + truePath->edgePredicate + ")";
  }

  // Reconstruct a selected objective by conjoining the incremental predicate
  // at every prefix. In particular PC(p.F) = PC(p) AND NOT(edge(p.T)).
  std::optional<std::string>
  resolvePathCondition(const std::string &label) const {
    if (!containsNode(label)) {
      return std::nullopt;
    }
    if (label.empty()) {
      return "true";
    }
    std::string result;
    size_t end = 0;
    do {
      end = label.find('.', end);
      const std::string prefix = label.substr(0, end);
      const auto predicate = resolveEdgePredicate(prefix);
      if (!predicate) {
        return std::nullopt;
      }
      if (!result.empty()) {
        result += " and ";
      }
      result += "(" + *predicate + ")";
      if (end != std::string::npos) {
        ++end;
      }
    } while (end != std::string::npos);
    return result;
  }

  std::vector<std::string> getExitPathLabels() const {
    std::vector<std::string> labels;
    for (const auto &[label, path] : nodes_) {
      if (path.reachesExit) {
        labels.push_back(label);
      }
    }
    return labels;
  }

  auto begin() const { return nodes_.begin(); }
  auto end() const { return nodes_.end(); }
  auto begin() { return nodes_.begin(); }
  auto end() { return nodes_.end(); }
};

#endif
