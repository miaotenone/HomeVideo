#pragma once

// JSON helpers shared by the hub client and the on-disk manifest, so the
// detection shape is serialised in exactly one place.

#include <optional>
#include <string>
#include <vector>

#include "hv/types.h"
#include "json.hpp"

namespace hv {

inline nlohmann::json detection_to_json(const Detection& detection) {
  nlohmann::json out;
  out["label"] = to_wire(detection.label);
  out["confidence"] = detection.confidence;
  if (detection.bbox.has_value()) {
    out["bbox"] = {detection.bbox->x, detection.bbox->y, detection.bbox->w,
                   detection.bbox->h};
  } else {
    out["bbox"] = nullptr;
  }
  if (detection.zone.has_value()) {
    out["zone"] = *detection.zone;
  } else {
    out["zone"] = nullptr;
  }
  return out;
}

inline nlohmann::json detections_to_json(const std::vector<Detection>& detections) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& detection : detections) {
    out.push_back(detection_to_json(detection));
  }
  return out;
}

inline std::optional<Detection> detection_from_json(const nlohmann::json& node) {
  if (!node.is_object()) return std::nullopt;
  const auto label = node.find("label");
  if (label == node.end() || !label->is_string()) return std::nullopt;
  const auto type = event_type_from_wire(label->get<std::string>());
  if (!type.has_value()) return std::nullopt;

  Detection detection;
  detection.label = *type;
  if (node.contains("confidence") && node["confidence"].is_number()) {
    detection.confidence = node["confidence"].get<float>();
  }
  if (node.contains("bbox") && node["bbox"].is_array() && node["bbox"].size() == 4) {
    BBox box;
    box.x = node["bbox"][0].get<int>();
    box.y = node["bbox"][1].get<int>();
    box.w = node["bbox"][2].get<int>();
    box.h = node["bbox"][3].get<int>();
    detection.bbox = box;
  }
  if (node.contains("zone") && node["zone"].is_string()) {
    detection.zone = node["zone"].get<std::string>();
  }
  return detection;
}

}  // namespace hv
