#pragma once

#include "ir.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace mdr {

void emitVerilog(const ModuleIR& module, const std::filesystem::path& directory,
                 const std::filesystem::path& programImage = {});

namespace rtl {
struct PipelineLayout {
    unsigned rfPorts;
    unsigned deBits = 1;
    unsigned emBits = 1;
};

std::array<std::string, 3> controllers(const ModuleIR& module, PipelineLayout& layout);
std::string top(const PipelineLayout& layout);
std::string hex32(std::uint32_t value);
}

} // namespace mdr

