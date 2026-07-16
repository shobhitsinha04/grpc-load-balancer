// Tiny --key=value parser, shared by both binaries.
//
// Not using gflags/absl::flags on purpose: two binaries with a handful of
// options each do not justify the dependency, and keeping the argument surface
// this small means the k8s manifests and demo script stay readable.

#pragma once

#include <string>
#include <vector>

namespace common {

inline std::string FlagValue(int argc, char** argv, const std::string& name,
                             const std::string& fallback) {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  }
  return fallback;
}

inline int FlagInt(int argc, char** argv, const std::string& name, int fallback) {
  const std::string raw = FlagValue(argc, argv, name, "");
  if (raw.empty()) return fallback;
  try {
    return std::stoi(raw);
  } catch (const std::exception&) {
    return fallback;
  }
}

inline std::vector<std::string> Split(const std::string& text, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == delim) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace common
