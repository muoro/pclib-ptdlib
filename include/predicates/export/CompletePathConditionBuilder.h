#ifndef COMPLETE_PATH_CONDITION_BUILDER_H
#define COMPLETE_PATH_CONDITION_BUILDER_H

#include <optional>
#include <string>
#include <vector>

#include "predicates/model/CompletePathCondition.h"
#include "predicates/model/PathNode.h"

class CompletePathConditionBuilder {
public:
  // Resolve exactly the requested objectives, preserving their first occurrence.
  // An empty selection returns an empty vector. Any unresolved objective fails
  // the entire request instead of silently omitting a path.
  static std::optional<std::vector<CompletePathCondition>>
  build(const PathNodeSet &nodes, const std::vector<std::string> &labels,
        std::string &error);
};

#endif
