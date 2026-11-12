#include "predicates/library/PCLibrary.h"
#include "predicates/model/PathLabel.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace {
bool isIdentifier(const std::string &value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
    return false;
  for (char ch : value)
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
      return false;
  return true;
}

std::set<std::string> parentsOf(const PathNodeSet &paths) {
  std::set<std::string> parents;
  for (const auto &[label, path] : paths)
    if (!label.empty())
      parents.insert(PathLabels::parentLabel(label));
  return parents;
}

std::string endpoint(const std::string &label, const PathNode &path,
                     const std::set<std::string> &parents) {
  if (path.reachesExit)
    return "exit";
  return parents.count(label) ? "prefix" : "cutoff";
}

struct SummaryCounts {
  int64_t totalNodes = 0;
  int64_t completePaths = 0;
  int64_t storedPredicates = 0;
};

SummaryCounts summarize(const PathNodeSet &paths) {
  SummaryCounts summary;
  for (const auto &[label, path] : paths) {
    ++summary.totalNodes;
    summary.completePaths += path.reachesExit;
    summary.storedPredicates += path.hasStoredEdgePredicate();
  }
  return summary;
}

bool hexDigest(const std::string &value) {
  if (value.size() != 64)
    return false;
  for (char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}
} // namespace

std::string PCLibrary::sha256(const std::string &text) {
  llvm::SHA256 hash;
  hash.update(text);
  const auto digest = hash.final();
  const char *hex = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

std::string PCLibrary::libraryId() const {
  // Length-prefixed fields and ordered map traversal avoid JSON key-order
  // dependence and ambiguous concatenation. This identifies content, not trust.
  std::string canonical;
  const auto append = [&canonical](const std::string &value) {
    canonical += std::to_string(value.size()) + ":" + value;
  };
  // This is the content-hash format version, independent of the JSON schema.
  // Grouping the same analysis as schema v2 preserves existing PTDlib links.
  append(semantics == "signed-bv32-wrap" ? "pclib-1" : "pclib-2");
  append(semantics);
  append(functionName);
  append(sourceSha256);
  append(std::to_string(configuredMaxPasses));
  append(std::to_string(analysisPasses));
  append(std::to_string(inputNames.size()));
  for (const auto &name : inputNames)
    append(name);
  for (const auto &[label, path] : paths) {
    append(label);
    append(path.edgePredicate);
    append(path.reachesExit ? "exit" : "open");
  }
  return sha256(canonical);
}

bool PCLibrary::validate(std::string &error) const {
  error.clear();
  if (semantics != "signed-bv32-wrap" && semantics != "mathematical-integers") {
    error = "unsupported PClib arithmetic semantics";
    return false;
  }
  if (!isIdentifier(functionName) || !hexDigest(sourceSha256)) {
    error = "PClib requires a function identifier and a source SHA-256 digest";
    return false;
  }
  if (configuredMaxPasses == 0 || analysisPasses == 0 ||
      analysisPasses > configuredMaxPasses) {
    error = "invalid backward-propagation pass metadata";
    return false;
  }
  std::set<std::string> names;
  for (const auto &name : inputNames) {
    if (!isIdentifier(name) || !names.insert(name).second) {
      error = "invalid or duplicate input name: " + name;
      return false;
    }
  }
  const auto *root = paths.findNode("");
  if (!root || root->hasStoredEdgePredicate()) {
    error = "PClib requires a root with an empty label and no edge predicate";
    return false;
  }
  const auto parents = parentsOf(paths);
  for (const auto &[label, path] : paths) {
    if (!PathLabels::isValid(label)) {
      error = "invalid path label: " + label;
      return false;
    }
    if (path.reachesExit && parents.count(label)) {
      error = "an exit path cannot have a represented continuation: " + label;
      return false;
    }
    if (label.empty())
      continue;
    if (!paths.findNode(PathLabels::parentLabel(label))) {
      error = "missing parent for path: " + label;
      return false;
    }
    if (PathLabels::isTrueEdge(label)) {
      if (!path.hasStoredEdgePredicate()) {
        error = "true edge has no incremental predicate: " + label;
        return false;
      }
      // Reject label references and undeclared inputs even in export-only mode.
      // Operators are checked by the solver parser when a target is solved.
      for (size_t pos = 0; pos < path.edgePredicate.size();) {
        const unsigned char ch = path.edgePredicate[pos];
        if (!std::isalpha(ch) && ch != '_') {
          ++pos;
          continue;
        }
        const size_t start = pos++;
        while (pos < path.edgePredicate.size() &&
               (std::isalnum(
                    static_cast<unsigned char>(path.edgePredicate[pos])) ||
                path.edgePredicate[pos] == '_' ||
                path.edgePredicate[pos] == '.'))
          ++pos;
        const std::string token = path.edgePredicate.substr(start, pos - start);
        if (!names.count(token) && token != "true" && token != "false" &&
            token != "and" && token != "not") {
          error = "undeclared symbol or label reference '" + token +
                  "' in edge " + label;
          return false;
        }
      }
    } else {
      const std::string trueLabel = PathLabels::trueSiblingLabel(label);
      const auto *sibling = paths.findNode(trueLabel);
      if (path.hasStoredEdgePredicate() || !sibling ||
          !sibling->hasStoredEdgePredicate()) {
        error =
            "false edge must negate its same-context true sibling: " + label;
        return false;
      }
    }
  }
  return true;
}

bool PCLibrary::writeJSON(const std::string &filename,
                          std::string &error) const {
  if (!validate(error))
    return false;
  const auto parents = parentsOf(paths);
  const auto summary = summarize(paths);
  std::error_code ec;
  llvm::raw_fd_ostream output(filename, ec);
  if (ec) {
    error = "cannot write PClib: " + ec.message();
    return false;
  }
  // Explicit object framing preserves the reading order. Use the existing
  // formatv JSON serializer for every key/value, including all source strings;
  // constructing json::OStream directly is unreliable with this LLVM build.
  const auto field = [&output](unsigned indent, llvm::StringRef name,
                               const llvm::json::Value &value, bool comma) {
    const std::string encoded = llvm::formatv("{0:2}", value).str();
    output.indent(indent);
    output << llvm::formatv("{0}", llvm::json::Value(name)) << ": ";
    for (char ch : encoded) {
      output << ch;
      if (ch == '\n')
        output.indent(indent);
    }
    output << (comma ? ",\n" : "\n");
  };
  output << "{\n  \"summary\": {\n";
  field(4, "total_nodes", summary.totalNodes, true);
  field(4, "complete_paths", summary.completePaths, true);
  field(4, "stored_predicates", summary.storedPredicates, false);
  output << "  },\n  \"metadata\": {\n";
  field(4, "schema", "pclib", true);
  field(4, "schema_version", semantics == "signed-bv32-wrap" ? 2 : 3, true);
  field(4, "library_id", libraryId(), true);
  field(4, "source_sha256", sourceSha256, false);
  output << "  },\n  \"analysis\": {\n";
  field(4, "semantics", semantics, true);
  output << "    \"bound\": {\n";
  field(6, "kind", "backward-propagation-passes", true);
  field(6, "configured", static_cast<int64_t>(configuredMaxPasses), true);
  field(6, "performed", static_cast<int64_t>(analysisPasses), false);
  output << "    }\n  },\n  \"function\": {\n";
  field(4, "name", functionName, true);
  llvm::json::Array inputs;
  for (const auto &name : inputNames)
    inputs.push_back(llvm::json::Object{{"name", name}, {"type", "int32"}});
  field(4, "inputs", std::move(inputs), false);
  output << "  },\n  \"nodes\": [\n";
  bool firstNode = true;
  for (const auto &[label, path] : paths) {
    if (!firstNode)
      output << ",\n";
    firstNode = false;
    output << "    {\n";
    field(6, "label", label, true);
    if (label.empty())
      field(6, "parent", nullptr, true);
    else
      field(6, "parent", PathLabels::parentLabel(label), true);
    field(6, "endpoint", endpoint(label, path, parents), !label.empty());
    if (!label.empty()) {
      if (PathLabels::isTrueEdge(label)) {
        field(6, "edge_condition", path.edgePredicate, false);
      } else {
        const std::string sibling = PathLabels::trueSiblingLabel(label);
        field(6, "negated_from", sibling, false);
      }
    }
    output << "    }";
  }
  output << "\n  ]\n}\n";
  output.close();
  if (output.has_error()) {
    error = "failed while writing PClib";
    return false;
  }
  return true;
}

std::optional<PCLibrary> PCLibrary::readJSON(const std::string &filename,
                                             std::string &error) {
  error.clear();
  const auto fail =
      [&error](const std::string &message) -> std::optional<PCLibrary> {
    error = message;
    return std::nullopt;
  };
  auto buffer = llvm::MemoryBuffer::getFile(filename);
  if (!buffer)
    return fail("cannot open PClib: " + buffer.getError().message());
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed)
    return fail("invalid PClib JSON: " + llvm::toString(parsed.takeError()));
  const auto *root = parsed->getAsObject();
  if (!root)
    return fail("PClib must be a JSON object");

  const llvm::json::Object *metadata = root;
  const llvm::json::Object *analysis = root;
  const llvm::json::Object *functionObject = nullptr;
  const llvm::json::Object *summary = nullptr;
  const bool grouped = root->get("metadata") != nullptr;
  if (grouped) {
    metadata = root->getObject("metadata");
    analysis = root->getObject("analysis");
    functionObject = root->getObject("function");
    summary = root->getObject("summary");
    if (!metadata || !analysis || !functionObject || !summary)
      return fail("PClib v2 requires summary, metadata, analysis and "
                  "function objects");
    for (const char *key : {"schema", "schema_version", "library_id",
                            "source_sha256", "semantics", "bound", "inputs"})
      if (root->get(key))
        return fail("PClib cannot mix flat v1 and grouped v2 fields");
  } else if (root->get("summary") || root->get("analysis")) {
    return fail("PClib cannot mix flat v1 and grouped v2 fields");
  }
  const auto semantics = analysis->getString("semantics");
  if (semantics == "signed-bv32-no-overflow")
    return fail("checked-arithmetic PClib is retired; extract a new library from source");
  const bool integers = semantics == "mathematical-integers";
  if (metadata->getString("schema") != "pclib" ||
      metadata->getInteger("schema_version") != (integers ? 3 : (grouped ? 2 : 1)) ||
      (integers && !grouped) || (!integers && semantics != "signed-bv32-wrap"))
    return fail("unsupported PClib schema, version or arithmetic semantics");

  const auto function = grouped ? functionObject->getString("name")
                                : root->getString("function");
  const auto source = metadata->getString("source_sha256");
  const auto id = metadata->getString("library_id");
  const auto *inputs = grouped ? functionObject->getArray("inputs")
                              : root->getArray("inputs");
  const auto *nodes = root->getArray("nodes");
  const auto *bound = analysis->getObject("bound");
  if (!function || !source || !id || !inputs || !nodes || !bound ||
      bound->getString("kind") != "backward-propagation-passes")
    return fail("missing PClib metadata, inputs, nodes or pass-bound kind");
  if (summary) {
    for (const char *key : {"total_nodes", "complete_paths", "stored_predicates"}) {
      const auto count = summary->getInteger(key);
      if (!count || *count < 0)
        return fail("PClib summary counts must be nonnegative integers");
    }
  }
  const auto configured = bound->getInteger("configured");
  const auto performed = bound->getInteger("performed");
  if (!configured || !performed || *configured <= 0 || *performed <= 0 ||
      *configured > std::numeric_limits<unsigned>::max() ||
      *performed > *configured)
    return fail("invalid PClib propagation bounds");
  PCLibrary library;
  library.semantics = semantics->str();
  library.functionName = function->str();
  library.sourceSha256 = source->str();
  library.configuredMaxPasses = static_cast<unsigned>(*configured);
  library.analysisPasses = static_cast<unsigned>(*performed);
  for (const auto &item : *inputs) {
    const auto *input = item.getAsObject();
    if (!input || !input->getString("name") ||
        input->getString("type") != "int32")
      return fail("PClib inputs must be named int32 values");
    library.inputNames.push_back(input->getString("name")->str());
  }
  std::set<std::string> seen;
  std::map<std::string, std::string> endpoints;
  for (const auto &item : *nodes) {
    const auto *node = item.getAsObject();
    if (!node || !node->getString("label") || !node->getString("endpoint"))
      return fail("PClib node requires label and endpoint");
    const std::string label = node->getString("label")->str();
    const std::string end = node->getString("endpoint")->str();
    if (!PathLabels::isValid(label) || !seen.insert(label).second ||
        (end != "exit" && end != "prefix" && end != "cutoff"))
      return fail("invalid or duplicate PClib node");
    if (label.empty()) {
      const auto *parent = node->get("parent");
      if (!parent || parent->kind() != llvm::json::Value::Null ||
          node->get("edge_condition") || node->get("negated_from"))
        return fail("invalid PClib root");
    } else {
      const auto parent = node->getString("parent");
      if (!parent || *parent != PathLabels::parentLabel(label))
        return fail("invalid PClib parent for " + label);
      if (PathLabels::isTrueEdge(label)) {
        if (!node->getString("edge_condition") || node->get("negated_from"))
          return fail("true node requires only an edge_condition");
      } else {
        const std::string sibling = PathLabels::trueSiblingLabel(label);
        if (node->getString("negated_from") != sibling ||
            node->get("edge_condition"))
          return fail(
              "false node must reference its same-context true sibling");
      }
    }
    auto &path = library.paths.getOrCreateNode(label);
    if (const auto predicate = node->getString("edge_condition"))
      path.edgePredicate = predicate->str();
    path.reachesExit = end == "exit";
    endpoints[label] = end;
  }
  if (!seen.count(""))
    return fail("PClib root node is missing");
  if (!library.validate(error))
    return std::nullopt;
  const auto parents = parentsOf(library.paths);
  for (const auto &[label, path] : library.paths)
    if (endpoints[label] != endpoint(label, path, parents))
      return fail("endpoint disagrees with PClib topology: " + label);
  if (*id != library.libraryId())
    return fail("PClib library_id does not match its content");
  if (summary) {
    const auto expected = summarize(library.paths);
    if (summary->getInteger("total_nodes") != expected.totalNodes ||
        summary->getInteger("complete_paths") != expected.completePaths ||
        summary->getInteger("stored_predicates") != expected.storedPredicates)
      return fail("PClib summary does not match its nodes");
  }
  return library;
}
