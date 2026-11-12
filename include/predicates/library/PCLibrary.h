#ifndef PC_LIBRARY_H
#define PC_LIBRARY_H

#include <optional>
#include <string>
#include <vector>

#include "predicates/model/PathNode.h"

// Persistent incremental constraints. An empty label is the root. Only T
// edges store a predicate; F edges negate the T edge in the same path context.
class PCLibrary {
public:
  std::string semantics = "mathematical-integers";
  std::string functionName;
  std::string sourceSha256;
  std::vector<std::string> inputNames;
  unsigned configuredMaxPasses = 1;
  unsigned analysisPasses = 1;
  PathNodeSet paths;

  static std::string sha256(const std::string &text);
  std::string libraryId() const;
  bool validate(std::string &error) const;
  // Writes grouped PClib v3 for mathematical integers and v2 for wrapping.
  bool writeJSON(const std::string &filename, std::string &error) const;
  // Reads mathematical-integer v3 and legacy wrapping v1/v2.
  static std::optional<PCLibrary> readJSON(const std::string &filename,
                                           std::string &error);
};

#endif
