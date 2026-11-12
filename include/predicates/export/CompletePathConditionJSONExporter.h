#ifndef COMPLETE_PATH_CONDITION_JSON_EXPORTER_H
#define COMPLETE_PATH_CONDITION_JSON_EXPORTER_H

#include <string>
#include <vector>

#include "predicates/model/CompletePathCondition.h"

class CompletePathConditionJSONExporter {
public:
  static bool exportToJSON(const std::vector<CompletePathCondition> &paths,
                           const std::string &functionName,
                           unsigned configuredMaxPasses,
                           unsigned analysisPasses,
                           const std::string &outputFilename);
};

#endif // COMPLETE_PATH_CONDITION_JSON_EXPORTER_H
