#pragma once

#include <string>

namespace unit_identity {

inline constexpr char kProjectCode[] = "EZQ";
inline constexpr char kBoardCode[] = "CTLR";
inline constexpr char kBoardRevision[] = "B";
inline constexpr char kBoardId[] = "EZQ-CTLR-B";

void init();
std::string factory_mac_string();
std::string unit_id();
bool has_unit_id();

}  // namespace unit_identity
