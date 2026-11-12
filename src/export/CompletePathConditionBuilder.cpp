#include "predicates/export/CompletePathConditionBuilder.h"

#include <set>

std::optional<std::vector<CompletePathCondition>>
CompletePathConditionBuilder::build(const PathNodeSet &nodes,
                                    const std::vector<std::string> &labels,
                                    std::string &error) {
  error.clear();
  std::set<std::string> seen;
  std::vector<CompletePathCondition> conditions;
  for (const auto &label : labels) {
    if (!seen.insert(label).second)
      continue;
    const auto predicate = nodes.resolvePathCondition(label);
    if (!predicate) {
      error = "Cannot resolve target path '" + label + "'";
      return std::nullopt;
    }
    conditions.push_back({label, *predicate});
  }
  return conditions;
}
