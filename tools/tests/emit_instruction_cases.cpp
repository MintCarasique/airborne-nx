#include "recompiler.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream input(argv[1]);
    struct Case { std::string name; uint32_t code; };
    std::vector<Case> cases;
    std::string name;
    uint32_t code;
    while (input >> name >> std::hex >> code) cases.push_back({name, code});
    if (cases.empty()) return 3;
    Recompiler compiler;
    compiler.image.base = 0x10000;
    compiler.image.size = uint32_t(cases.size() * 8);
    compiler.image.data = std::make_unique<uint8_t[]>(compiler.image.size);
    compiler.image.Map(".text", 0, compiler.image.size, SectionFlags_Code, compiler.image.data.get());
    for (size_t i = 0; i < cases.size(); ++i) {
        auto* dest = compiler.image.data.get() + i * 8;
        const uint32_t words[] = {cases[i].code, 0x4e800020};
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 4; ++k) dest[j*4+k] = uint8_t(words[j] >> (24-k*8));
        compiler.image.symbols.emplace(cases[i].name, 0x10000+i*8, 8, Symbol_Function);
    }
    compiler.out = "#include \"ppc_context.h\"\n";
    for (size_t i = 0; i < cases.size(); ++i) {
        if (!compiler.Recompile(Function{0x10000+i*8, 8})) {
            std::cerr << "Incomplete translation: " << cases[i].name << '\n';
            return 4;
        }
    }
    std::ofstream output(argv[2]);
    output << compiler.out;
    return output ? 0 : 5;
}
