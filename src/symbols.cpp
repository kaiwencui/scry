#include "symbols.hpp"
#include "simd.hpp"

namespace scry {

namespace {

inline bool is_ident_start(u8 c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_ident_char(u8 c) noexcept {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

inline std::string_view skip_ws(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

inline std::string_view read_ident(std::string_view s, size_t& pos) noexcept {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size() || !is_ident_start(static_cast<u8>(s[pos]))) return {};
    size_t start = pos;
    while (pos < s.size() && is_ident_char(static_cast<u8>(s[pos]))) ++pos;
    return s.substr(start, pos - start);
}

bool is_keyword_not_func(std::string_view w) noexcept {
    return w == "if" || w == "for" || w == "while" || w == "switch" || w == "catch" ||
           w == "return" || w == "sizeof" || w == "alignof" || w == "decltype" ||
           w == "static_assert" || w == "else" || w == "do" || w == "case" || w == "new" ||
           w == "delete" || w == "throw" || w == "typedef" || w == "using";
}

void emit_sym(std::vector<ExtractedSymbol>& out, std::string_view name, u32 line_no, SymbolKind kind) {
    if (name.size() < 2 || name.size() > 128) return;
    if (out.size() >= 4096) return; // Per-file safety cap
    out.push_back({std::string(name), line_no, kind});
}

} // namespace

const char* symbol_kind_name(SymbolKind k) noexcept {
    switch (k) {
        case SymbolKind::Function: return "fn";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Class: return "class";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Macro: return "macro";
        case SymbolKind::Trait: return "trait";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::TypeAlias: return "type";
        case SymbolKind::Const: return "const";
        default: return "sym";
    }
}

void extract_symbols(const u8* data, size_t len, LangId lang, std::vector<ExtractedSymbol>& out) {
    out.clear();
    if (len == 0) return;

    // Only extract symbols from code files
    switch (lang) {
        case LangId::C:
        case LangId::Cpp:
        case LangId::Rust:
        case LangId::Python:
        case LangId::Go:
        case LangId::JavaScript:
        case LangId::TypeScript:
        case LangId::Java:
        case LangId::Zig:
        case LangId::Swift:
        case LangId::Kotlin:
        case LangId::Ruby:
        case LangId::Shell:
            break;
        default:
            return;
    }

    size_t pos = 0;
    u32 line_no = 1;
    while (pos < len) {
        size_t nl = simd::find_byte(data + pos, len - pos, '\n');
        size_t end = pos + nl;
        size_t tend = (end > pos && data[end - 1] == '\r') ? (end - 1) : end;
        std::string_view raw(reinterpret_cast<const char*>(data + pos), tend - pos);
        pos = (end < len) ? (end + 1) : len;
        u32 cur_line = line_no++;

        std::string_view line = skip_ws(raw);
        if (line.size() < 4) continue;
        // Skip comments
        if (line[0] == '/' && (line[1] == '/' || line[1] == '*')) continue;
        if (line[0] == '*' || line[0] == ';') continue;

        if (lang == LangId::C || lang == LangId::Cpp) {
            if (line[0] == '#') {
                std::string_view rest = skip_ws(line.substr(1));
                if (rest.rfind("define", 0) == 0 && rest.size() > 7 && (rest[6] == ' ' || rest[6] == '\t')) {
                    size_t p = 7;
                    auto name = read_ident(rest, p);
                    emit_sym(out, name, cur_line, SymbolKind::Macro);
                }
                continue;
            }
            // struct / class / enum / namespace / using
            size_t p = 0;
            auto kw = read_ident(line, p);
            if (kw == "template") continue;
            if (kw == "struct" || kw == "class" || kw == "enum") {
                SymbolKind sk = (kw == "struct") ? SymbolKind::Struct
                              : (kw == "class") ? SymbolKind::Class : SymbolKind::Enum;
                auto next = read_ident(line, p);
                if (kw == "enum" && (next == "class" || next == "struct")) {
                    next = read_ident(line, p);
                }
                size_t brace = line.find('{', p);
                size_t semi = line.find(';', p);
                if (!next.empty() && (semi == std::string_view::npos || (brace != std::string_view::npos && brace < semi))) {
                    emit_sym(out, next, cur_line, sk);
                }
                continue;
            }
            if (kw == "using") {
                auto name = read_ident(line, p);
                if (!name.empty() && line.find('=', p) != std::string_view::npos) {
                    emit_sym(out, name, cur_line, SymbolKind::TypeAlias);
                }
                continue;
            }
            // Top-level or method function definition heuristic:
            // Contains '(' and does NOT end with ';' (avoid declarations/calls),
            // and line starts at col 0 or after visibility/qualifier.
            if (raw.size() > 0 && raw[0] != ' ' && raw[0] != '\t' &&
                line.find('(') != std::string_view::npos && line.back() != ';') {
                size_t lparen = line.find('(');
                // Walk backwards from lparen to find identifier
                size_t e = lparen;
                while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
                size_t s = e;
                while (s > 0 && is_ident_char(static_cast<u8>(line[s - 1]))) --s;
                if (e > s && is_ident_start(static_cast<u8>(line[s]))) {
                    std::string_view fn = line.substr(s, e - s);
                    if (!is_keyword_not_func(fn)) {
                        emit_sym(out, fn, cur_line, SymbolKind::Function);
                    }
                }
            }
        } else if (lang == LangId::Rust) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            while (kw == "pub" || kw == "async" || kw == "unsafe" || kw == "const" || kw == "extern") {
                if (p < line.size() && line[p] == '(') {
                    size_t close = line.find(')', p);
                    if (close == std::string_view::npos) break;
                    p = close + 1;
                }
                kw = read_ident(line, p);
            }
            if (kw == "fn") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
            else if (kw == "struct") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Struct);
            else if (kw == "enum") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Enum);
            else if (kw == "trait") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Trait);
            else if (kw == "type") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::TypeAlias);
            else if (kw == "macro_rules") {
                if (p < line.size() && line[p] == '!') ++p;
                emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Macro);
            }
        } else if (lang == LangId::Python) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            if (kw == "async") kw = read_ident(line, p);
            if (kw == "def") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
            else if (kw == "class") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Class);
        } else if (lang == LangId::Go) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            if (kw == "func") {
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
                if (p < line.size() && line[p] == '(') {
                    size_t close = line.find(')', p);
                    if (close != std::string_view::npos) p = close + 1;
                }
                emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
            } else if (kw == "type") {
                auto name = read_ident(line, p);
                auto kind_kw = read_ident(line, p);
                SymbolKind sk = SymbolKind::TypeAlias;
                if (kind_kw == "struct") sk = SymbolKind::Struct;
                else if (kind_kw == "interface") sk = SymbolKind::Interface;
                emit_sym(out, name, cur_line, sk);
            }
        } else if (lang == LangId::JavaScript || lang == LangId::TypeScript) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            while (kw == "export" || kw == "default" || kw == "async" || kw == "declare" || kw == "abstract") {
                kw = read_ident(line, p);
            }
            if (kw == "function") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
            else if (kw == "class") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Class);
            else if (kw == "interface") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Interface);
            else if (kw == "type") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::TypeAlias);
            else if (kw == "enum") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Enum);
        } else if (lang == LangId::Zig) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            if (kw == "pub") kw = read_ident(line, p);
            if (kw == "fn") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
        } else if (lang == LangId::Java || lang == LangId::Kotlin || lang == LangId::Swift) {
            size_t p = 0;
            auto kw = read_ident(line, p);
            while (kw == "public" || kw == "private" || kw == "protected" || kw == "internal" ||
                   kw == "static" || kw == "final" || kw == "abstract" || kw == "open" || kw == "data") {
                kw = read_ident(line, p);
            }
            if (kw == "class") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Class);
            else if (kw == "interface" || kw == "protocol") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Interface);
            else if (kw == "enum") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Enum);
            else if (kw == "struct") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Struct);
            else if (kw == "func" || kw == "fun") emit_sym(out, read_ident(line, p), cur_line, SymbolKind::Function);
        }
    }
}

} // namespace scry
