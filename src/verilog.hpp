#pragma once

#include "ir.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace mdr {

void emitVerilog(const ModuleIR& module, const std::filesystem::path& directory,
                 const std::filesystem::path& programImage = {});

namespace rtl {
std::string lane(const InstructionIR& ir, unsigned index, unsigned ports);
std::string top(unsigned instructionCount, unsigned ports);
std::string hex32(std::uint32_t value);
}

} // namespace mdr

