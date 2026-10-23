#include <image.h>
#include <file.h>
#include <fstream>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    auto bytes = LoadFile(argv[1]);
    if (bytes.size() < 24) return 3;
    auto image = Image::ParseImage(bytes.data(), bytes.size());
    if (!image.data || !image.size) return 4;
    const std::filesystem::path out(argv[2]);
    std::filesystem::create_directories(out);
    std::ofstream memory(out / "image_loader_normalized.bin", std::ios::binary);
    memory.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    std::ofstream sections(out / "sections.tsv");
    sections << "name\tbase\tsize\tflags\n";
    for (const auto& s : image.sections)
        sections << s.name.substr(0, 8) << '\t' << std::hex << s.base << '\t' << s.size << '\t' << unsigned(s.flags) << '\n';
    std::ofstream imports(out / "import_symbols.tsv");
    imports << "name\taddress\tsize\n";
    for (const auto& s : image.symbols)
        imports << s.name << '\t' << std::hex << s.address << '\t' << s.size << '\n';
    std::cout << "Loaded " << image.size << " bytes, " << image.sections.size()
              << " sections, " << image.symbols.size() << " named import thunks\n";
    return memory && sections && imports ? 0 : 5;
}
