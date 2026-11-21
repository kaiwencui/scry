#pragma once

#include "common.hpp"
#include "gitignore.hpp"
#include "index_format.hpp"

#include <string>
#include <vector>

namespace scry {

struct ExtractedSymbol {
    std::string name;
    u32 line_no = 0;
    SymbolKind kind = SymbolKind::Unknown;
};

// Lightweight lexical definition/symbol extractor (no full AST/LSP needed).
// Extracts functions, structs, classes, enums, traits, interfaces, macros, types.
void extract_symbols(const u8* data, size_t len, LangId lang, std::vector<ExtractedSymbol>& out);

const char* symbol_kind_name(SymbolKind k) noexcept;

} // namespace scry
