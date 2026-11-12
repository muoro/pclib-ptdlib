#ifndef PREDICATES_MODEL_PATH_LABEL_H
#define PREDICATES_MODEL_PATH_LABEL_H

#include <cassert>
#include <cctype>
#include <cstddef>
#include <string>

// Serialized labels use B<block-id>T/F components separated by dots.
// The empty label denotes the root. These helpers have no Clang/Z3 dependency.
namespace PathLabels {
inline bool isValid(const std::string &label) {
  size_t pos = 0;
  while (pos < label.size()) {
    if (label[pos++] != 'B')
      return false;
    const size_t digits = pos;
    while (pos < label.size() &&
           std::isdigit(static_cast<unsigned char>(label[pos])))
      ++pos;
    if (pos == digits || pos == label.size() ||
        (label[pos] != 'T' && label[pos] != 'F'))
      return false;
    ++pos;
    if (pos == label.size())
      return true;
    if (label[pos++] != '.' || pos == label.size())
      return false;
  }
  return true;
}

inline bool isTrueEdge(const std::string &label) {
  return !label.empty() && label.back() == 'T';
}

inline bool isFalseEdge(const std::string &label) {
  return !label.empty() && label.back() == 'F';
}

inline std::string parentLabel(const std::string &label) {
  const size_t dot = label.rfind('.');
  return dot == std::string::npos ? "" : label.substr(0, dot);
}

// Requires a valid non-root branch label. Earlier decisions remain unchanged.
inline std::string trueSiblingLabel(const std::string &label) {
  assert(!label.empty() && isValid(label));
  std::string sibling = label;
  sibling.back() = 'T';
  return sibling;
}

inline std::string prepend(const std::string &prefix,
                           const std::string &suffix) {
  if (prefix.empty())
    return suffix;
  return suffix.empty() ? prefix : prefix + "." + suffix;
}
} // namespace PathLabels

#endif
