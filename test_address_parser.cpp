#include <iostream>
#include <iomanip>
#include <vector>
#include "include/address_parser.h"

using namespace Haywire;

void TestParse(AddressParser& parser, const std::string& input) {
    ParsedAddress result = parser.Parse(input);
    
    std::cout << "Input: \"" << input << "\"" << std::endl;
    std::cout << "  Valid: " << (result.isValid ? "YES" : "NO") << std::endl;
    std::cout << "  Address: 0x" << std::hex << result.address << std::dec << std::endl;
    std::cout << "  Space: " << AddressParser::GetSpacePrefix(result.space) << std::endl;
    if (!result.warning.empty()) {
        std::cout << "  Warning: " << result.warning << std::endl;
    }
    std::cout << "  Confidence: " << result.confidence << std::endl;
    std::cout << std::endl;
}

int main() {
    AddressParser parser;
    
    // Set some variables
    parser.SetVariable("base", 0x40000000);
    parser.SetVariable("offset", 0x1000);
    
    std::cout << "=== Address Parser Tests ===" << std::endl << std::endl;
    
    // Test space prefixes
    std::cout << "--- Space Prefixes ---" << std::endl;
    TestParse(parser, "s:1000");
    TestParse(parser, "p:40000000");
    TestParse(parser, "v:7fff8000");
    TestParse(parser, "c:8000");
    
    // Test number formats
    std::cout << "--- Number Formats ---" << std::endl;
    TestParse(parser, "0x1234");
    TestParse(parser, "1234h");
    TestParse(parser, "$DEAD");
    TestParse(parser, ".256");
    TestParse(parser, "1000.");
    TestParse(parser, "100d");
    
    // Test arithmetic
    std::cout << "--- Arithmetic ---" << std::endl;
    TestParse(parser, "p:40000000+100");
    TestParse(parser, "s:1000-10");
    TestParse(parser, "$base+$offset");
    TestParse(parser, "$base+.256");
    
    // Test PID-qualified VA
    std::cout << "--- PID-Qualified ---" << std::endl;
    TestParse(parser, "v:1234:7fff8000");
    TestParse(parser, "v:5678:stack");
    
    // Test error cases
    std::cout << "--- Error Cases ---" << std::endl;
    TestParse(parser, "garbage");
    TestParse(parser, "");
    TestParse(parser, "x:1000");  // Invalid prefix
    
    // Test AddressDisplayer
    std::cout << "=== Address Displayer Tests ===" << std::endl << std::endl;
    
    AddressDisplayer displayer;
    
    AddressDisplayer::DisplayInfo info = displayer.GetDisplay(
        0x40001000, AddressSpace::PHYSICAL,
        64, 32,  // x, y
        256, 3   // stride, bytesPerPixel
    );
    
    std::cout << "Formula: " << info.formula << std::endl;
    std::cout << "Simplified: " << info.simplified << std::endl;
    std::cout << "Base: 0x" << std::hex << info.base << std::dec << std::endl;
    std::cout << "Offset: " << info.offset << std::endl;
    std::cout << "Row: " << info.row << ", Col: " << info.col << std::endl;
    
    return 0;
}