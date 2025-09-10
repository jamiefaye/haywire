#include "include/address_parser.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Haywire {

AddressParser::AddressParser() 
    : builtinRamBase(0x40000000),
      builtinStackPtr(0),
      builtinProgramCounter(0) {
    // Set default built-in variables
    variables["ram"] = builtinRamBase;
    variables["base"] = 0;
}

AddressParser::~AddressParser() {}

ParsedAddress AddressParser::Parse(const std::string& input) {
    return Parse(input, AddressSpace::NONE, -1);
}

ParsedAddress AddressParser::Parse(const std::string& input, AddressSpace currentSpace, int currentPid) {
    std::string trimmed = Trim(input);
    if (trimmed.empty()) {
        ParsedAddress result;
        result.warning = "Empty input";
        return result;
    }
    
    // Try parsing with prefix first (s:, p:, v:, c:)
    ParsedAddress result = ParsePrefixedAddress(trimmed);
    if (result.isValid) {
        return result;
    }
    
    // Try parsing as expression with arithmetic
    result = ParseExpression(trimmed, currentSpace);
    if (result.isValid) {
        return result;
    }
    
    // Try parsing as simple number
    uint64_t value;
    bool defaultHex = (currentSpace != AddressSpace::NONE);  // Use hex for addresses
    if (ParseNumber(trimmed, value, defaultHex)) {
        result.address = value;
        result.space = currentSpace;
        result.isValid = true;
        result.confidence = 0.8;
        return result;
    }
    
    // Failed to parse - return best guess
    result.address = 0;
    result.space = currentSpace;
    result.isValid = false;
    result.warning = "Could not parse: " + trimmed;
    result.confidence = 0.0;
    return result;
}

ParsedAddress AddressParser::ParsePrefixedAddress(const std::string& input) {
    ParsedAddress result;
    
    // Check for space prefix
    if (input.length() >= 2 && input[1] == ':') {
        char prefix = input[0];
        std::string remainder = input.substr(2);
        
        switch (prefix) {
            case 's': case 'S':
                result.space = AddressSpace::SHARED;
                break;
            case 'p': case 'P':
                result.space = AddressSpace::PHYSICAL;
                break;
            case 'v': case 'V': {
                result.space = AddressSpace::VIRTUAL;
                // Check for PID qualifier (v:1234:address)
                size_t colonPos = remainder.find(':');
                if (colonPos != std::string::npos) {
                    std::string pidStr = remainder.substr(0, colonPos);
                    remainder = remainder.substr(colonPos + 1);
                    char* endptr;
                    long pid = strtol(pidStr.c_str(), &endptr, 10);
                    if (*endptr == '\0') {
                        result.hasPid = true;
                        result.pid = pid;
                    }
                }
                break;
            }
            case 'c': case 'C':
                result.space = AddressSpace::CRUNCHED;
                break;
            default:
                return result;  // Invalid prefix
        }
        
        // Parse the remainder as a number or expression
        if (ParseNumber(remainder, result.address, true)) {
            result.isValid = true;
            result.confidence = 1.0;
            return result;
        }
        
        // Try as expression
        ParsedAddress exprResult = ParseExpression(remainder, result.space);
        if (exprResult.isValid) {
            result.address = exprResult.address;
            result.isValid = true;
            result.confidence = 0.9;
            return result;
        }
    }
    
    return result;
}

ParsedAddress AddressParser::ParseExpression(const std::string& input, AddressSpace currentSpace) {
    ParsedAddress result;
    result.space = currentSpace;
    
    // Look for + or - operators
    size_t plusPos = input.find('+');
    size_t minusPos = input.find('-');
    
    if (plusPos != std::string::npos) {
        std::string left = Trim(input.substr(0, plusPos));
        std::string right = Trim(input.substr(plusPos + 1));
        
        uint64_t leftVal, rightVal;
        
        // Parse left side (could be variable)
        if (left[0] == '$' && HasVariable(left.substr(1))) {
            leftVal = GetVariable(left.substr(1));
        } else if (!ParseNumber(left, leftVal, true)) {
            return result;
        }
        
        // Parse right side
        if (!ParseNumber(right, rightVal, false)) {
            return result;
        }
        
        result.address = leftVal + rightVal;
        result.isValid = true;
        result.confidence = 0.9;
        return result;
    }
    
    if (minusPos != std::string::npos && minusPos > 0) {  // Not negative number
        std::string left = Trim(input.substr(0, minusPos));
        std::string right = Trim(input.substr(minusPos + 1));
        
        uint64_t leftVal, rightVal;
        
        // Parse left side
        if (left[0] == '$' && HasVariable(left.substr(1))) {
            leftVal = GetVariable(left.substr(1));
        } else if (!ParseNumber(left, leftVal, true)) {
            return result;
        }
        
        // Parse right side
        if (!ParseNumber(right, rightVal, false)) {
            return result;
        }
        
        result.address = leftVal - rightVal;
        result.isValid = true;
        result.confidence = 0.9;
        return result;
    }
    
    // Check for variable
    if (input[0] == '$' && HasVariable(input.substr(1))) {
        result.address = GetVariable(input.substr(1));
        result.isValid = true;
        result.confidence = 1.0;
        return result;
    }
    
    // Check for special symbols
    if (input == "stack") {
        result.address = builtinStackPtr;
        result.space = AddressSpace::VIRTUAL;
        result.isValid = true;
        result.confidence = 1.0;
        return result;
    }
    
    return result;
}

bool AddressParser::ParseNumber(const std::string& str, uint64_t& value, bool defaultHex) {
    if (str.empty()) return false;
    
    // Check for decimal with period
    if (str[0] == '.' || str[str.length()-1] == '.') {
        return ParseDecimalNumber(str, value);
    }
    
    // Check for explicit hex prefixes
    if (str.length() >= 2) {
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return ParseHexNumber(str.substr(2), value);
        }
        if (str[0] == '$') {
            return ParseHexNumber(str.substr(1), value);
        }
    }
    
    // Check for hex suffix
    if (str[str.length()-1] == 'h' || str[str.length()-1] == 'H') {
        return ParseHexNumber(str.substr(0, str.length()-1), value);
    }
    
    // Check for decimal suffix
    if (str[str.length()-1] == 'd' || str[str.length()-1] == 'D') {
        return ParseDecimalNumber(str.substr(0, str.length()-1), value);
    }
    
    // Default interpretation
    if (defaultHex) {
        return ParseHexNumber(str, value);
    } else {
        return ParseDecimalNumber(str, value);
    }
}

bool AddressParser::ParseHexNumber(const std::string& str, uint64_t& value) {
    if (str.empty()) return false;
    
    char* endptr;
    value = strtoull(str.c_str(), &endptr, 16);
    return *endptr == '\0';
}

bool AddressParser::ParseDecimalNumber(const std::string& str, uint64_t& value) {
    std::string cleaned = str;
    
    // Remove period if present
    if (!cleaned.empty() && cleaned[0] == '.') {
        cleaned = cleaned.substr(1);
    } else if (!cleaned.empty() && cleaned[cleaned.length()-1] == '.') {
        cleaned = cleaned.substr(0, cleaned.length()-1);
    }
    
    if (cleaned.empty()) return false;
    
    char* endptr;
    value = strtoull(cleaned.c_str(), &endptr, 10);
    return *endptr == '\0';
}

std::string AddressParser::Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

void AddressParser::SetVariable(const std::string& name, uint64_t value) {
    variables[name] = value;
}

uint64_t AddressParser::GetVariable(const std::string& name) const {
    auto it = variables.find(name);
    return (it != variables.end()) ? it->second : 0;
}

bool AddressParser::HasVariable(const std::string& name) const {
    return variables.find(name) != variables.end();
}

void AddressParser::SetBuiltins(uint64_t ramBase, uint64_t stackPtr, uint64_t programCounter) {
    builtinRamBase = ramBase;
    builtinStackPtr = stackPtr;
    builtinProgramCounter = programCounter;
    
    variables["ram"] = ramBase;
    variables["sp"] = stackPtr;
    variables["stack"] = stackPtr;
    variables["pc"] = programCounter;
}

std::string AddressParser::GetSpacePrefix(AddressSpace space) {
    switch (space) {
        case AddressSpace::SHARED:   return "s:";
        case AddressSpace::PHYSICAL: return "p:";
        case AddressSpace::VIRTUAL:  return "v:";
        case AddressSpace::CRUNCHED: return "c:";
        default: return "";
    }
}

AddressSpace AddressParser::ParseSpacePrefix(const std::string& prefix) {
    if (prefix == "s:" || prefix == "S:") return AddressSpace::SHARED;
    if (prefix == "p:" || prefix == "P:") return AddressSpace::PHYSICAL;
    if (prefix == "v:" || prefix == "V:") return AddressSpace::VIRTUAL;
    if (prefix == "c:" || prefix == "C:") return AddressSpace::CRUNCHED;
    return AddressSpace::NONE;
}

std::string AddressParser::Format(uint64_t address, AddressSpace space) {
    std::stringstream ss;
    ss << GetSpacePrefix(space);
    ss << std::hex << address;
    return ss.str();
}

// AddressDisplayer implementation

AddressDisplayer::AddressDisplayer() 
    : memoryMapper(nullptr), qemuConnection(nullptr) {
}

AddressDisplayer::~AddressDisplayer() {}

AddressDisplayer::DisplayInfo AddressDisplayer::GetDisplay(uint64_t address, AddressSpace space,
                                                           int x, int y, int stride, int bytesPerPixel) {
    DisplayInfo info;
    
    // Calculate the offset
    uint64_t offset = y * stride + x * bytesPerPixel;
    uint64_t base = address - offset;
    
    info.base = base;
    info.offset = offset;
    info.row = y;
    info.col = x;
    info.stride = stride;
    info.bytesPerPixel = bytesPerPixel;
    
    // Build the formula
    std::stringstream formula;
    formula << AddressParser::GetSpacePrefix(space) << std::hex << base;
    
    if (offset > 0) {
        formula << " + ";
        if (y > 0) {
            formula << "(" << std::dec << y << " * " << stride;
            if (x > 0) {
                formula << " + " << x;
            }
            formula << ")";
            if (bytesPerPixel > 1) {
                formula << " * " << bytesPerPixel;
            }
        } else if (x > 0) {
            formula << std::dec << x;
            if (bytesPerPixel > 1) {
                formula << " * " << bytesPerPixel;
            }
        }
    }
    
    info.formula = formula.str();
    
    // Simplified result
    std::stringstream simple;
    simple << AddressParser::GetSpacePrefix(space) << std::hex << address;
    info.simplified = simple.str();
    
    // Get all spaces
    info.allSpaces = GetAllSpaces(address, space);
    
    return info;
}

std::string AddressDisplayer::GetAllSpaces(uint64_t address, AddressSpace currentSpace) {
    std::stringstream ss;
    
    // For now, just show the current space
    // TODO: Implement translation between spaces
    ss << AddressParser::Format(address, currentSpace);
    
    return ss.str();
}

uint64_t AddressDisplayer::TranslateAddress(uint64_t addr, AddressSpace from, AddressSpace to) {
    // TODO: Implement actual translation using memoryMapper and qemuConnection
    // For now, return the same address
    return addr;
}

}  // namespace Haywire