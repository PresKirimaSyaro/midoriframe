#include "verilog.hpp"
#include "rtl_resources.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace mdr {
namespace {

template<std::size_t N>
std::string_view resource(const unsigned char (&bytes)[N]) {
    return {reinterpret_cast<const char*>(bytes), N};
}

void writeFile(const std::filesystem::path& file, std::string_view text) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write " + file.string());
}

std::string readImage(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read program image " + file.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Release the obsolete generated controller when regenerating a v0.1 directory.
std::string_view legacyController(const std::filesystem::path& directory) {
    std::ifstream manifest(directory / "files.f", std::ios::binary);
    const std::string files{std::istreambuf_iterator<char>(manifest), std::istreambuf_iterator<char>()};
    if (files == "mips.v\nISA.v\nIM.v\nDM.v\nRF.v\nmips_tb.v\n") {
        std::ifstream old(directory / "ISA.v", std::ios::binary);
        const std::string controller{std::istreambuf_iterator<char>(old), std::istreambuf_iterator<char>()};
        if (controller.starts_with("`include \"Promise.vh\"\n") &&
            (controller.find("\nmodule ISA(\n") != std::string::npos ||
             controller.find("\nmodule mdr_instruction_0(\n") != std::string::npos))
            return "ISA.v";
    }
    if (files == "mips.v\nController.v\nFDreg.v\nDEreg.v\nEMreg.v\nMWreg.v\nIM.v\nDM.v\nRF.v\nmips_tb.v\n") {
        std::ifstream old(directory / "Controller.v", std::ios::binary);
        const std::string controller{std::istreambuf_iterator<char>(old), std::istreambuf_iterator<char>()};
        if (controller.starts_with("`include \"Promise.vh\"\n\nmodule Controller(\n"))
            return "Controller.v";
    }
    return {};
}


} // namespace

namespace rtl {
std::string hex32(std::uint32_t value) {
    std::ostringstream stream;
    stream << "32'h" << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}
} // namespace rtl

void emitVerilog(const ModuleIR& module, const std::filesystem::path& directory,
                 const std::filesystem::path& programImage) {
    const auto ports = std::max(1u, module.readUpperBound);
    rtl::PipelineLayout layout{ports};
    const auto controllers = rtl::controllers(module, layout);
    const auto top = rtl::top(layout);
    const auto installImage = !programImage.empty() || !std::filesystem::exists(directory / "code.txt");
    std::string image;
    if (!programImage.empty()) {
        image = readImage(programImage);
    } else if (installImage) {
        for (unsigned i = 0; i < 4096; ++i) image += "00000000\n";
    }

    const auto obsolete = legacyController(directory);
    std::filesystem::create_directories(directory);
    writeFile(directory / "mips.v", top);
    writeFile(directory / "DCtrl.v", controllers[0]);
    writeFile(directory / "ECtrl.v", controllers[1]);
    writeFile(directory / "MCtrl.v", controllers[2]);
    writeFile(directory / "STALL.v", resource(rtl::STALL_v));
    writeFile(directory / "IM.v", resource(rtl::IM_v));
    writeFile(directory / "DM.v", resource(rtl::DM_v));
    writeFile(directory / "RF.v", resource(rtl::RF_v));
    writeFile(directory / "FDreg.v", resource(rtl::FDreg_v));
    writeFile(directory / "DEreg.v", resource(rtl::DEreg_v));
    writeFile(directory / "EMreg.v", resource(rtl::EMreg_v));
    writeFile(directory / "MWreg.v", resource(rtl::MWreg_v));
    writeFile(directory / "Promise.vh", resource(rtl::Promise_vh));
    writeFile(directory / "mips_tb.v", resource(rtl::mips_tb_v));
    writeFile(directory / "files.f", "mips.v\nDCtrl.v\nECtrl.v\nMCtrl.v\nSTALL.v\nFDreg.v\nDEreg.v\nEMreg.v\nMWreg.v\nIM.v\nDM.v\nRF.v\nmips_tb.v\n");
    if (installImage) writeFile(directory / "code.txt", image);
    if (!obsolete.empty()) std::filesystem::remove(directory / obsolete);
}

} // namespace mdr

