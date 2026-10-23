// Standalone host frontend. Dependency revision and Airborne patch are pinned.
#include "shader_recompiler.h"
#include <fstream>
#include <iterator>
#include <iostream>

static std::string read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 16*1024*1024)
        throw std::runtime_error("Cannot read bounded shader input");
    std::string data(size_t(file.tellg()), '\0');
    file.seekg(0);
    if (!file.read(data.data(), data.size())) throw std::runtime_error("Short read");
    return data;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc != 4) return 2;
    try {
        auto data = read(argv[1]), common = read(argv[3]);
        if (data.size() < sizeof(ShaderContainer)) return 2;
        const auto* header = reinterpret_cast<const ShaderContainer*>(data.data());
        if (uint64_t(header->virtualSize) + uint64_t(header->physicalSize) != data.size()) return 2;
        // Python frontend performs full bounded table/CF validation first.
        ShaderRecompiler compiler;
        compiler.recompile(reinterpret_cast<const uint8_t*>(data.data()), common);
        std::ofstream output(argv[2], std::ios::binary);
        if (!output.write(compiler.out.data(), compiler.out.size())) return 3;
        std::cout << "{\"spec_constants_mask\":" << compiler.specConstantsMask << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
