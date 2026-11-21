#include "regex_engine.hpp"
#include "simd.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace scry {

namespace {

inline bool is_word_byte(u8 c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

inline bool is_word_boundary_at(std::string_view line, size_t pos) noexcept {
    bool left = (pos > 0) && is_word_byte(static_cast<u8>(line[pos - 1]));
    bool right = (pos < line.size()) && is_word_byte(static_cast<u8>(line[pos]));
    return left != right;
}

std::string to_lower_str(std::string_view s) {
    std::string out(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(ascii_lower(static_cast<u8>(s[i])));
    }
    return out;
}

bool has_ascii_upper(std::string_view s) noexcept {
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') return true;
    }
    return false;
}

// ============================================================
// Regex AST Representation & Recursive-Descent Parser
// ============================================================
struct AstNode {
    enum class Kind : u8 {
        Empty,
        Literal,
        Class,
        Concat,
        Alt,
        Repeat,
        Anchor
    };
    enum class AnchorKind : u8 {
        StartLine,
        EndLine,
        WordBoundary,
        NonWordBoundary
    };

    Kind kind = Kind::Empty;
    u8 ch = 0;
    bool icase = false;
    ByteSet bset;
    std::vector<std::unique_ptr<AstNode>> children;
    u32 rep_min = 0;
    u32 rep_max = 0; // UINT32_MAX means unbounded
    bool greedy = true;
    AnchorKind anchor = AnchorKind::StartLine;
};

using AstPtr = std::unique_ptr<AstNode>;

ByteSet make_digit_set() {
    ByteSet s;
    for (u8 c = '0'; c <= '9'; ++c) s.set(c);
    return s;
}

ByteSet make_word_set() {
    ByteSet s;
    for (u8 c = 'a'; c <= 'z'; ++c) s.set(c);
    for (u8 c = 'A'; c <= 'Z'; ++c) s.set(c);
    for (u8 c = '0'; c <= '9'; ++c) s.set(c);
    s.set('_');
    return s;
}

ByteSet make_space_set() {
    ByteSet s;
    s.set(' ');
    s.set('\t');
    s.set('\r');
    s.set('\n');
    s.set('\f');
    s.set('\v');
    return s;
}

ByteSet make_dot_set(bool dot_matches_nl) {
    ByteSet s;
    s.invert();
    if (!dot_matches_nl) s.clear('\n');
    return s;
}

void apply_icase_to_byteset(ByteSet& s) {
    for (u8 c = 'a'; c <= 'z'; ++c) {
        u8 u = static_cast<u8>(c - 32);
        if (s.test(c) || s.test(u)) {
            s.set(c);
            s.set(u);
        }
    }
}

class RegexParser {
public:
    RegexParser(std::string_view pat, bool icase)
        : pat_(pat), icase_(icase) {}

    AstPtr parse(std::string& err) {
        auto node = parse_alt(err);
        if (!err.empty()) return nullptr;
        if (pos_ != pat_.size()) {
            err = "unexpected trailing character in regex";
            return nullptr;
        }
        return node;
    }

private:
    bool eof() const noexcept { return pos_ >= pat_.size(); }
    char peek() const noexcept { return pat_[pos_]; }
    char get() noexcept { return pat_[pos_++]; }

    AstPtr parse_alt(std::string& err) {
        std::vector<AstPtr> branches;
        branches.push_back(parse_concat(err));
        if (!err.empty()) return nullptr;
        while (!eof() && peek() == '|') {
            get();
            branches.push_back(parse_concat(err));
            if (!err.empty()) return nullptr;
        }
        if (branches.size() == 1) return std::move(branches[0]);
        auto n = std::make_unique<AstNode>();
        n->kind = AstNode::Kind::Alt;
        n->children = std::move(branches);
        return n;
    }

    AstPtr parse_concat(std::string& err) {
        std::vector<AstPtr> seq;
        while (!eof() && peek() != '|' && peek() != ')') {
            auto atom = parse_quantified(err);
            if (!err.empty()) return nullptr;
            if (atom && atom->kind != AstNode::Kind::Empty) {
                seq.push_back(std::move(atom));
            }
        }
        if (seq.empty()) return std::make_unique<AstNode>();
        if (seq.size() == 1) return std::move(seq[0]);
        auto n = std::make_unique<AstNode>();
        n->kind = AstNode::Kind::Concat;
        n->children = std::move(seq);
        return n;
    }

    AstPtr parse_quantified(std::string& err) {
        auto atom = parse_atom(err);
        if (!err.empty() || !atom) return nullptr;

        while (!eof()) {
            char c = peek();
            u32 rmin = 0, rmax = 0;
            bool is_rep = false;
            if (c == '*') {
                get();
                rmin = 0;
                rmax = UINT32_MAX;
                is_rep = true;
            } else if (c == '+') {
                get();
                rmin = 1;
                rmax = UINT32_MAX;
                is_rep = true;
            } else if (c == '?') {
                get();
                rmin = 0;
                rmax = 1;
                is_rep = true;
            } else if (c == '{') {
                size_t save = pos_;
                if (parse_brace_quantifier(rmin, rmax)) {
                    is_rep = true;
                } else {
                    pos_ = save;
                    break;
                }
            }
            if (!is_rep) break;

            bool greedy = true;
            if (!eof() && peek() == '?') {
                get();
                greedy = false;
            }
            auto rep = std::make_unique<AstNode>();
            rep->kind = AstNode::Kind::Repeat;
            rep->rep_min = rmin;
            rep->rep_max = rmax;
            rep->greedy = greedy;
            rep->children.push_back(std::move(atom));
            atom = std::move(rep);
        }
        return atom;
    }

    bool parse_brace_quantifier(u32& rmin, u32& rmax) {
        if (eof() || peek() != '{') return false;
        get();
        if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) return false;
        u32 a = 0;
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
            a = a * 10 + static_cast<u32>(get() - '0');
            if (a > 1000) return false;
        }
        if (eof()) return false;
        if (peek() == '}') {
            get();
            rmin = rmax = a;
            return true;
        }
        if (peek() != ',') return false;
        get();
        if (eof()) return false;
        if (peek() == '}') {
            get();
            rmin = a;
            rmax = UINT32_MAX;
            return true;
        }
        if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
        u32 b = 0;
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
            b = b * 10 + static_cast<u32>(get() - '0');
            if (b > 1000) return false;
        }
        if (eof() || peek() != '}' || b < a) return false;
        get();
        rmin = a;
        rmax = b;
        return true;
    }

    AstPtr parse_atom(std::string& err) {
        char c = get();
        if (c == '.') {
            auto n = std::make_unique<AstNode>();
            n->kind = AstNode::Kind::Class;
            n->bset = make_dot_set(dot_nl_);
            return n;
        }
        if (c == '^') {
            auto n = std::make_unique<AstNode>();
            n->kind = AstNode::Kind::Anchor;
            n->anchor = AstNode::AnchorKind::StartLine;
            return n;
        }
        if (c == '$') {
            auto n = std::make_unique<AstNode>();
            n->kind = AstNode::Kind::Anchor;
            n->anchor = AstNode::AnchorKind::EndLine;
            return n;
        }
        if (c == '(') {
            bool saved_icase = icase_;
            bool saved_dot = dot_nl_;
            if (!eof() && peek() == '?') {
                get();
                if (!eof() && peek() == ':') {
                    get();
                } else {
                    // Inline flags (?i), (?-i), (?s), (?i:...)
                    bool neg = false;
                    while (!eof() && peek() != ')' && peek() != ':') {
                        char fc = get();
                        if (fc == '-') { neg = true; }
                        else if (fc == 'i') { icase_ = !neg; }
                        else if (fc == 's') { dot_nl_ = !neg; }
                        else if (fc == 'm') { /* line-by-line is already multiline */ }
                        else {
                            err = "unsupported inline flag in group";
                            return nullptr;
                        }
                    }
                    if (!eof() && peek() == ')') {
                        get();
                        // Standalone flag modifier (?i) persists in enclosing scope
                        return std::make_unique<AstNode>();
                    }
                    if (!eof() && peek() == ':') {
                        get();
                    }
                }
            }
            auto inner = parse_alt(err);
            if (!err.empty()) return nullptr;
            if (eof() || peek() != ')') {
                err = "unclosed '(' in regex";
                return nullptr;
            }
            get();
            icase_ = saved_icase;
            dot_nl_ = saved_dot;
            return inner;
        }
        if (c == '[') {
            return parse_char_class(err);
        }
        if (c == '\\') {
            return parse_escape(err);
        }
        if (c == '*' || c == '+' || c == '?') {
            err = "dangling quantifier in regex";
            return nullptr;
        }
        auto n = std::make_unique<AstNode>();
        n->kind = AstNode::Kind::Literal;
        n->ch = static_cast<u8>(c);
        n->icase = icase_;
        return n;
    }

    int hex_val(char c) const noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    AstPtr parse_escape(std::string& err) {
        if (eof()) {
            err = "trailing backslash in regex";
            return nullptr;
        }
        char c = get();
        auto make_class = [](ByteSet s) {
            auto n = std::make_unique<AstNode>();
            n->kind = AstNode::Kind::Class;
            n->bset = s;
            return n;
        };
        auto make_lit = [&](u8 b) {
            auto n = std::make_unique<AstNode>();
            n->kind = AstNode::Kind::Literal;
            n->ch = b;
            n->icase = icase_;
            return n;
        };
        switch (c) {
            case 'd': return make_class(make_digit_set());
            case 'D': { auto s = make_digit_set(); s.invert(); s.clear('\n'); return make_class(s); }
            case 'w': return make_class(make_word_set());
            case 'W': { auto s = make_word_set(); s.invert(); return make_class(s); }
            case 's': return make_class(make_space_set());
            case 'S': { auto s = make_space_set(); s.invert(); return make_class(s); }
            case 'b': {
                auto n = std::make_unique<AstNode>();
                n->kind = AstNode::Kind::Anchor;
                n->anchor = AstNode::AnchorKind::WordBoundary;
                return n;
            }
            case 'B': {
                auto n = std::make_unique<AstNode>();
                n->kind = AstNode::Kind::Anchor;
                n->anchor = AstNode::AnchorKind::NonWordBoundary;
                return n;
            }
            case 'A': {
                auto n = std::make_unique<AstNode>();
                n->kind = AstNode::Kind::Anchor;
                n->anchor = AstNode::AnchorKind::StartLine;
                return n;
            }
            case 'z':
            case 'Z': {
                auto n = std::make_unique<AstNode>();
                n->kind = AstNode::Kind::Anchor;
                n->anchor = AstNode::AnchorKind::EndLine;
                return n;
            }
            case 'n': return make_lit('\n');
            case 'r': return make_lit('\r');
            case 't': return make_lit('\t');
            case 'f': return make_lit('\f');
            case 'v': return make_lit('\v');
            case 'x': {
                if (pos_ + 2 <= pat_.size()) {
                    int h1 = hex_val(pat_[pos_]);
                    int h2 = hex_val(pat_[pos_ + 1]);
                    if (h1 >= 0 && h2 >= 0) {
                        pos_ += 2;
                        return make_lit(static_cast<u8>((h1 << 4) | h2));
                    }
                }
                return make_lit('x');
            }
            default:
                return make_lit(static_cast<u8>(c));
        }
    }

    AstPtr parse_char_class(std::string& err) {
        bool neg = false;
        if (!eof() && peek() == '^') {
            get();
            neg = true;
        }
        ByteSet s;
        bool first = true;
        while (!eof()) {
            if (!first && peek() == ']') {
                get();
                if (icase_) apply_icase_to_byteset(s);
                if (neg) {
                    s.invert();
                    s.clear('\n'); // Line-oriented regex: [^a] does not match '\n'
                }
                auto n = std::make_unique<AstNode>();
                n->kind = AstNode::Kind::Class;
                n->bset = s;
                return n;
            }
            first = false;

            u8 lo = 0;
            bool is_shorthand = false;
            if (peek() == '\\') {
                get();
                if (eof()) {
                    err = "unclosed character class";
                    return nullptr;
                }
                char esc = get();
                if (esc == 'd') { s.merge(make_digit_set()); is_shorthand = true; }
                else if (esc == 'D') { auto t = make_digit_set(); t.invert(); s.merge(t); is_shorthand = true; }
                else if (esc == 'w') { s.merge(make_word_set()); is_shorthand = true; }
                else if (esc == 'W') { auto t = make_word_set(); t.invert(); s.merge(t); is_shorthand = true; }
                else if (esc == 's') { s.merge(make_space_set()); is_shorthand = true; }
                else if (esc == 'S') { auto t = make_space_set(); t.invert(); s.merge(t); is_shorthand = true; }
                else if (esc == 'n') lo = '\n';
                else if (esc == 'r') lo = '\r';
                else if (esc == 't') lo = '\t';
                else lo = static_cast<u8>(esc);
            } else {
                lo = static_cast<u8>(get());
            }

            if (is_shorthand) continue;

            if (!eof() && peek() == '-' && pos_ + 1 < pat_.size() && pat_[pos_ + 1] != ']') {
                get(); // '-'
                u8 hi = 0;
                if (peek() == '\\') {
                    get();
                    if (eof()) { err = "unclosed character class"; return nullptr; }
                    char esc = get();
                    if (esc == 'n') hi = '\n';
                    else if (esc == 'r') hi = '\r';
                    else if (esc == 't') hi = '\t';
                    else hi = static_cast<u8>(esc);
                } else {
                    hi = static_cast<u8>(get());
                }
                if (hi < lo) {
                    err = "invalid range in character class";
                    return nullptr;
                }
                for (u32 c = lo; c <= hi; ++c) s.set(static_cast<u8>(c));
            } else {
                s.set(lo);
            }
        }
        err = "unclosed '[' in regex";
        return nullptr;
    }

    std::string_view pat_;
    size_t pos_ = 0;
    bool icase_ = false;
    bool dot_nl_ = false;
};

// ============================================================
// Trigram Query Algebra (Russ Cox / Code Search Style)
// ============================================================

constexpr size_t kMaxExactSet = 32;
constexpr size_t kMaxAffixSet = 32;

struct AnalyzeInfo {
    bool emptyable = false;
    bool exact_valid = false; // If true, `exact` contains ALL possible matching strings
    std::vector<std::string> exact;
    std::vector<std::string> prefix; // Every match starts with one of these
    std::vector<std::string> suffix; // Every match ends with one of these
    TrigramQuery query = TrigramQuery::make_all();
    bool any_icase = false;
};

void dedup_sort(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

TrigramQuery trigrams_from_string(std::string_view s, bool icase) {
    if (s.size() < 3) return TrigramQuery::make_all();
    std::vector<TrigramQuery> atoms;
    atoms.reserve(s.size() - 2);
    for (size_t i = 0; i + 3 <= s.size(); ++i) {
        Trigram t = pack_trigram(static_cast<u8>(s[i]),
                                 static_cast<u8>(s[i + 1]),
                                 static_cast<u8>(s[i + 2]));
        atoms.push_back(TrigramQuery::make_atom(t, icase));
    }
    return TrigramQuery::make_and(std::move(atoms));
}

TrigramQuery trigrams_from_set(const std::vector<std::string>& strs, bool icase) {
    if (strs.empty()) return TrigramQuery::make_none();
    std::vector<TrigramQuery> branches;
    branches.reserve(strs.size());
    for (const auto& s : strs) {
        if (s.size() < 3) return TrigramQuery::make_all();
        branches.push_back(trigrams_from_string(s, icase));
    }
    return TrigramQuery::make_or(std::move(branches));
}

std::vector<std::string> trim_prefixes(std::vector<std::string> v) {
    dedup_sort(v);
    if (v.size() > kMaxAffixSet) {
        // Shorten to 2 chars or collapse to {""}
        std::vector<std::string> sh;
        for (auto& s : v) {
            sh.push_back(s.size() > 2 ? s.substr(0, 2) : s);
        }
        dedup_sort(sh);
        if (sh.size() > kMaxAffixSet) return {""};
        return sh;
    }
    return v;
}

std::vector<std::string> trim_suffixes(std::vector<std::string> v) {
    dedup_sort(v);
    if (v.size() > kMaxAffixSet) {
        std::vector<std::string> sh;
        for (auto& s : v) {
            sh.push_back(s.size() > 2 ? s.substr(s.size() - 2) : s);
        }
        dedup_sort(sh);
        if (sh.size() > kMaxAffixSet) return {""};
        return sh;
    }
    return v;
}

AnalyzeInfo analyze_ast(const AstNode* node, bool global_icase) {
    AnalyzeInfo info;
    info.any_icase = global_icase;
    if (!node) {
        info.emptyable = true;
        info.exact_valid = true;
        info.exact = {""};
        info.prefix = {""};
        info.suffix = {""};
        return info;
    }

    switch (node->kind) {
        case AstNode::Kind::Empty:
        case AstNode::Kind::Anchor: {
            info.emptyable = true;
            info.exact_valid = true;
            info.exact = {""};
            info.prefix = {""};
            info.suffix = {""};
            return info;
        }
        case AstNode::Kind::Literal: {
            bool ic = global_icase || node->icase;
            info.any_icase = ic;
            info.exact_valid = true;
            std::string s(1, static_cast<char>(ic ? ascii_lower(node->ch) : node->ch));
            info.exact = {s};
            info.prefix = {s};
            info.suffix = {s};
            return info;
        }
        case AstNode::Kind::Class: {
            int cnt = node->bset.popcount();
            if (cnt == 0) {
                info.query = TrigramQuery::make_none();
                return info;
            }
            if (cnt <= 16) {
                info.exact_valid = true;
                for (u32 c = 0; c < 256; ++c) {
                    if (node->bset.test(static_cast<u8>(c))) {
                        char ch = static_cast<char>(global_icase ? ascii_lower(static_cast<u8>(c)) : c);
                        info.exact.emplace_back(1, ch);
                    }
                }
                dedup_sort(info.exact);
                info.prefix = info.exact;
                info.suffix = info.exact;
            } else {
                info.exact_valid = false;
                info.prefix = {""};
                info.suffix = {""};
            }
            return info;
        }
        case AstNode::Kind::Concat: {
            info.exact_valid = true;
            info.emptyable = true;
            info.exact = {""};
            info.prefix = {""};
            info.suffix = {""};
            info.query = TrigramQuery::make_all();

            for (const auto& ch : node->children) {
                AnalyzeInfo r = analyze_ast(ch.get(), global_icase);
                bool ic = info.any_icase || r.any_icase;
                info.any_icase = ic;

                // Combine required queries
                std::vector<TrigramQuery> ands;
                ands.push_back(std::move(info.query));
                ands.push_back(std::move(r.query));

                // Cross-boundary trigrams from info.suffix x r.prefix!
                if (!info.suffix.empty() && !r.prefix.empty() &&
                    info.suffix.size() * r.prefix.size() <= 64) {
                    std::vector<std::string> cross;
                    for (const auto& a : info.suffix) {
                        for (const auto& b : r.prefix) {
                            if (a.size() + b.size() >= 3) {
                                cross.push_back(a + b);
                            } else {
                                cross.push_back("");
                            }
                        }
                    }
                    ands.push_back(trigrams_from_set(cross, ic));
                }
                info.query = TrigramQuery::make_and(std::move(ands));

                // Update exact / prefix / suffix
                if (info.exact_valid && r.exact_valid &&
                    info.exact.size() * r.exact.size() <= kMaxExactSet) {
                    std::vector<std::string> comb;
                    comb.reserve(info.exact.size() * r.exact.size());
                    for (const auto& a : info.exact) {
                        for (const auto& b : r.exact) {
                            comb.push_back(a + b);
                        }
                    }
                    dedup_sort(comb);
                    info.exact = comb;
                    info.prefix = trim_prefixes(comb);
                    info.suffix = trim_suffixes(comb);
                    if (comb.size() <= kMaxExactSet) {
                        info.query = TrigramQuery::make_and({
                            std::move(info.query),
                            trigrams_from_set(comb, ic)
                        });
                    }
                } else {
                    // Prefix update
                    if (info.exact_valid && info.exact.size() * r.prefix.size() <= kMaxAffixSet) {
                        std::vector<std::string> np;
                        for (const auto& a : info.exact) {
                            for (const auto& b : r.prefix) np.push_back(a + b);
                        }
                        info.prefix = trim_prefixes(std::move(np));
                    }
                    // Suffix update
                    if (r.exact_valid && info.suffix.size() * r.exact.size() <= kMaxAffixSet) {
                        std::vector<std::string> ns;
                        for (const auto& a : info.suffix) {
                            for (const auto& b : r.exact) ns.push_back(a + b);
                        }
                        info.suffix = trim_suffixes(std::move(ns));
                    } else {
                        info.suffix = r.suffix;
                    }
                    info.exact_valid = false;
                    info.exact.clear();
                }
                info.emptyable = info.emptyable && r.emptyable;
            }
            return info;
        }
        case AstNode::Kind::Alt: {
            bool all_exact = true;
            std::vector<std::string> all_ex;
            std::vector<std::string> all_pre;
            std::vector<std::string> all_suf;
            std::vector<TrigramQuery> ors;
            bool ic = global_icase;
            for (const auto& ch : node->children) {
                AnalyzeInfo r = analyze_ast(ch.get(), global_icase);
                ic = ic || r.any_icase;
                if (r.emptyable) info.emptyable = true;
                if (r.exact_valid && all_exact && all_ex.size() + r.exact.size() <= kMaxExactSet) {
                    all_ex.insert(all_ex.end(), r.exact.begin(), r.exact.end());
                } else {
                    all_exact = false;
                }
                all_pre.insert(all_pre.end(), r.prefix.begin(), r.prefix.end());
                all_suf.insert(all_suf.end(), r.suffix.begin(), r.suffix.end());

                TrigramQuery bq = std::move(r.query);
                if (r.exact_valid) {
                    bq = TrigramQuery::make_and({std::move(bq), trigrams_from_set(r.exact, ic)});
                }
                ors.push_back(std::move(bq));
            }
            info.any_icase = ic;
            info.exact_valid = all_exact;
            if (all_exact) {
                dedup_sort(all_ex);
                info.exact = std::move(all_ex);
            }
            info.prefix = trim_prefixes(std::move(all_pre));
            info.suffix = trim_suffixes(std::move(all_suf));
            info.query = TrigramQuery::make_or(std::move(ors));
            return info;
        }
        case AstNode::Kind::Repeat: {
            AnalyzeInfo r = analyze_ast(node->children[0].get(), global_icase);
            info.any_icase = r.any_icase;
            if (node->rep_min == 0) {
                info.emptyable = true;
                info.exact_valid = false;
                info.prefix = {""};
                info.suffix = {""};
                info.query = TrigramQuery::make_all();
                return info;
            }
            // rep_min >= 1: at least one occurrence must appear!
            info.emptyable = r.emptyable;
            info.query = r.query;
            if (r.exact_valid) {
                info.query = TrigramQuery::make_and({
                    std::move(info.query),
                    trigrams_from_set(r.exact, r.any_icase)
                });
            }
            if (node->rep_min == 1 && node->rep_max == 1) {
                return r;
            }
            info.exact_valid = false;
            info.prefix = r.prefix;
            info.suffix = r.suffix;
            return info;
        }
    }
    return info;
}

// Extract longest required literal substring (for SIMD prefilter)
std::string extract_prefilter_literal(const AstNode* node, bool& icase_out) {
    if (!node) return {};
    switch (node->kind) {
        case AstNode::Kind::Literal: {
            if (node->icase) icase_out = true;
            return std::string(1, static_cast<char>(node->ch));
        }
        case AstNode::Kind::Concat: {
            std::string best;
            std::string cur;
            bool cur_ic = icase_out;
            for (const auto& ch : node->children) {
                if (ch->kind == AstNode::Kind::Literal) {
                    if (ch->icase) cur_ic = true;
                    cur.push_back(static_cast<char>(ch->ch));
                } else {
                    if (cur.size() > best.size()) {
                        best = cur;
                        icase_out = cur_ic;
                    }
                    cur.clear();
                    bool sub_ic = icase_out;
                    std::string sub = extract_prefilter_literal(ch.get(), sub_ic);
                    if (sub.size() > best.size()) {
                        best = std::move(sub);
                        icase_out = sub_ic;
                    }
                }
            }
            if (cur.size() > best.size()) {
                best = cur;
                icase_out = cur_ic;
            }
            return best;
        }
        case AstNode::Kind::Repeat: {
            if (node->rep_min >= 1) {
                return extract_prefilter_literal(node->children[0].get(), icase_out);
            }
            return {};
        }
        default:
            return {};
    }
}

bool ast_is_pure_literal(const AstNode* node, std::string& out, bool& any_icase) {
    if (!node) return true;
    switch (node->kind) {
        case AstNode::Kind::Empty:
            return true;
        case AstNode::Kind::Literal:
            if (node->icase) any_icase = true;
            out.push_back(static_cast<char>(node->ch));
            return true;
        case AstNode::Kind::Concat:
            for (const auto& ch : node->children) {
                if (!ast_is_pure_literal(ch.get(), out, any_icase)) return false;
            }
            return true;
        default:
            return false;
    }
}

bool ast_has_anchors(const AstNode* node) {
    if (!node) return false;
    if (node->kind == AstNode::Kind::Anchor) return true;
    for (const auto& ch : node->children) {
        if (ast_has_anchors(ch.get())) return true;
    }
    return false;
}

} // namespace

int ByteSet::popcount() const noexcept {
    int c = 0;
    for (u64 w : bits) c += static_cast<int>(std::popcount(w));
    return c;
}

bool ByteSet::single_char(u8& out) const noexcept {
    if (popcount() != 1) return false;
    for (int i = 0; i < 4; ++i) {
        if (bits[i] != 0) {
            out = static_cast<u8>((i << 6) + std::countr_zero(bits[i]));
            return true;
        }
    }
    return false;
}

TrigramQuery TrigramQuery::make_and(std::vector<TrigramQuery> ch) {
    std::vector<TrigramQuery> flat;
    std::set<std::pair<Trigram, bool>> seen_atoms;

    for (auto& c : ch) {
        if (c.kind == Kind::None) return make_none();
        if (c.kind == Kind::All) continue;
        if (c.kind == Kind::And) {
            for (auto& sub : c.children) {
                if (sub.kind == Kind::Atom) {
                    if (seen_atoms.insert({sub.trigram, sub.icase}).second) {
                        flat.push_back(std::move(sub));
                    }
                } else {
                    flat.push_back(std::move(sub));
                }
            }
        } else if (c.kind == Kind::Atom) {
            if (seen_atoms.insert({c.trigram, c.icase}).second) {
                flat.push_back(std::move(c));
            }
        } else {
            flat.push_back(std::move(c));
        }
    }
    if (flat.empty()) return make_all();
    if (flat.size() == 1) return std::move(flat[0]);
    return TrigramQuery{Kind::And, 0, false, std::move(flat)};
}

TrigramQuery TrigramQuery::make_or(std::vector<TrigramQuery> ch) {
    std::vector<TrigramQuery> flat;
    std::set<std::pair<Trigram, bool>> seen_atoms;

    for (auto& c : ch) {
        if (c.kind == Kind::All) return make_all();
        if (c.kind == Kind::None) continue;
        if (c.kind == Kind::Or) {
            for (auto& sub : c.children) {
                if (sub.kind == Kind::Atom) {
                    if (seen_atoms.insert({sub.trigram, sub.icase}).second) {
                        flat.push_back(std::move(sub));
                    }
                } else {
                    flat.push_back(std::move(sub));
                }
            }
        } else if (c.kind == Kind::Atom) {
            if (seen_atoms.insert({c.trigram, c.icase}).second) {
                flat.push_back(std::move(c));
            }
        } else {
            flat.push_back(std::move(c));
        }
    }
    if (flat.empty()) return make_none();
    if (flat.size() == 1) return std::move(flat[0]);

    // Factor common required Atoms present in EVERY branch of OR:
    // e.g. OR(AND(" sc","sch","che"), AND("\tsc","sch","che")) -> AND("sch","che", OR(" sc","\tsc"))
    auto branch_atoms = [](const TrigramQuery& b) -> std::set<std::pair<Trigram, bool>> {
        std::set<std::pair<Trigram, bool>> s;
        if (b.kind == Kind::Atom) {
            s.insert({b.trigram, b.icase});
        } else if (b.kind == Kind::And) {
            for (const auto& x : b.children) {
                if (x.kind == Kind::Atom) s.insert({x.trigram, x.icase});
            }
        }
        return s;
    };

    auto common = branch_atoms(flat[0]);
    for (size_t i = 1; i < flat.size() && !common.empty(); ++i) {
        auto ba = branch_atoms(flat[i]);
        std::set<std::pair<Trigram, bool>> inter;
        for (const auto& a : common) {
            if (ba.count(a)) inter.insert(a);
        }
        common.swap(inter);
    }

    if (!common.empty()) {
        std::vector<TrigramQuery> remainder;
        remainder.reserve(flat.size());
        bool collapsed_to_all = false;
        for (auto& b : flat) {
            if (b.kind == Kind::Atom) {
                if (common.count({b.trigram, b.icase})) {
                    collapsed_to_all = true;
                    break;
                }
                remainder.push_back(std::move(b));
            } else if (b.kind == Kind::And) {
                std::vector<TrigramQuery> kept;
                for (auto& x : b.children) {
                    if (x.kind == Kind::Atom && common.count({x.trigram, x.icase})) continue;
                    kept.push_back(std::move(x));
                }
                if (kept.empty()) {
                    collapsed_to_all = true;
                    break;
                }
                remainder.push_back(kept.size() == 1 ? std::move(kept[0])
                                                     : TrigramQuery{Kind::And, 0, false, std::move(kept)});
            } else {
                remainder.push_back(std::move(b));
            }
        }
        std::vector<TrigramQuery> and_terms;
        and_terms.reserve(common.size() + 1);
        for (const auto& [tri, ic] : common) {
            and_terms.push_back(make_atom(tri, ic));
        }
        if (!collapsed_to_all && !remainder.empty()) {
            if (remainder.size() == 1) and_terms.push_back(std::move(remainder[0]));
            else and_terms.push_back(TrigramQuery{Kind::Or, 0, false, std::move(remainder)});
        }
        return make_and(std::move(and_terms));
    }

    return TrigramQuery{Kind::Or, 0, false, std::move(flat)};
}

std::string TrigramQuery::to_string() const {
    switch (kind) {
        case Kind::All: return "ALL";
        case Kind::None: return "NONE";
        case Kind::Atom: {
            char buf[16];
            u8 a = static_cast<u8>((trigram >> 16) & 0xFF);
            u8 b = static_cast<u8>((trigram >> 8) & 0xFF);
            u8 c = static_cast<u8>(trigram & 0xFF);
            std::snprintf(buf, sizeof(buf), "\"%c%c%c\"%s",
                          std::isprint(a) ? a : '.',
                          std::isprint(b) ? b : '.',
                          std::isprint(c) ? c : '.',
                          icase ? "i" : "");
            return std::string(buf);
        }
        case Kind::And:
        case Kind::Or: {
            std::string s = (kind == Kind::And) ? "AND(" : "OR(";
            for (size_t i = 0; i < children.size(); ++i) {
                if (i > 0) s += ", ";
                s += children[i].to_string();
            }
            s += ")";
            return s;
        }
    }
    return "?";
}

// ============================================================
// Thompson NFA Compiler
// ============================================================
namespace {

struct Frag {
    u32 start = 0;
    std::vector<u32*> outs; // Pointers aren't safe across vector realloc; use indices!
};

struct PatchLoc {
    u32 state_idx = 0;
    bool is_out2 = false;
};

struct NfaFrag {
    u32 start = 0;
    std::vector<PatchLoc> outs;
};

} // namespace

bool CompiledPattern::compile(std::string_view pattern, const RegexOptions& opts, std::string& err) {
    static std::atomic<u64> g_next_pattern_id{1};
    pattern_id_ = g_next_pattern_id.fetch_add(1, std::memory_order_relaxed);
    err.clear();
    nfa_.clear();
    classes_.clear();
    literal_.clear();
    lower_literal_.clear();
    prefilter_.clear();

    word_regexp_ = opts.word_regexp;
    line_regexp_ = opts.line_regexp;

    bool ic = opts.case_insensitive;
    if (!ic && opts.smart_case) {
        ic = !has_ascii_upper(pattern);
    }
    effective_icase_ = ic;

    // Fast path: fixed-string mode (-F)
    if (opts.fixed_strings) {
        is_pure_literal_ = true;
        literal_ = std::string(pattern);
        lower_literal_ = to_lower_str(pattern);
        prefilter_ = literal_;
        prefilter_icase_ = ic;
        trigram_query_ = trigrams_from_string(pattern, ic);
        return true;
    }

    RegexParser parser(pattern, ic);
    AstPtr ast = parser.parse(err);
    if (!err.empty() || !ast) return false;

    // Check if regex is actually a pure literal!
    std::string pure_lit;
    bool lit_ic = ic;
    if (ast_is_pure_literal(ast.get(), pure_lit, lit_ic) && !pure_lit.empty()) {
        // Ensure all chars share same case mode (or global ic)
        is_pure_literal_ = true;
        effective_icase_ = lit_ic;
        literal_ = std::move(pure_lit);
        lower_literal_ = to_lower_str(literal_);
        prefilter_ = literal_;
        prefilter_icase_ = lit_ic;
        trigram_query_ = trigrams_from_string(literal_, lit_ic);
        return true;
    }

    is_pure_literal_ = false;

    // Analyze trigram query from AST
    AnalyzeInfo info = analyze_ast(ast.get(), ic);
    trigram_query_ = std::move(info.query);
    if (info.exact_valid && !info.exact.empty()) {
        trigram_query_ = TrigramQuery::make_and({
            std::move(trigram_query_),
            trigrams_from_set(info.exact, info.any_icase)
        });
    }

    // Extract SIMD prefilter literal (if >= 2 bytes)
    bool pre_ic = ic;
    std::string pre = extract_prefilter_literal(ast.get(), pre_ic);
    if (pre.size() >= 2) {
        prefilter_ = pre_ic ? to_lower_str(pre) : pre;
        prefilter_icase_ = pre_ic;
    }

    // Wrap AST with -w or -x anchors if requested
    if (word_regexp_) {
        auto wrap = std::make_unique<AstNode>();
        wrap->kind = AstNode::Kind::Concat;
        auto wb1 = std::make_unique<AstNode>();
        wb1->kind = AstNode::Kind::Anchor;
        wb1->anchor = AstNode::AnchorKind::WordBoundary;
        auto wb2 = std::make_unique<AstNode>();
        wb2->kind = AstNode::Kind::Anchor;
        wb2->anchor = AstNode::AnchorKind::WordBoundary;
        wrap->children.push_back(std::move(wb1));
        wrap->children.push_back(std::move(ast));
        wrap->children.push_back(std::move(wb2));
        ast = std::move(wrap);
    }
    if (line_regexp_) {
        auto wrap = std::make_unique<AstNode>();
        wrap->kind = AstNode::Kind::Concat;
        auto s1 = std::make_unique<AstNode>();
        s1->kind = AstNode::Kind::Anchor;
        s1->anchor = AstNode::AnchorKind::StartLine;
        auto s2 = std::make_unique<AstNode>();
        s2->kind = AstNode::Kind::Anchor;
        s2->anchor = AstNode::AnchorKind::EndLine;
        wrap->children.push_back(std::move(s1));
        wrap->children.push_back(std::move(ast));
        wrap->children.push_back(std::move(s2));
        ast = std::move(wrap);
    }

    // Build Thompson NFA from AST
    auto emit = [&](NfaState st) -> u32 {
        u32 id = static_cast<u32>(nfa_.size());
        nfa_.push_back(st);
        return id;
    };

    auto patch = [&](const std::vector<PatchLoc>& locs, u32 target) {
        for (const auto& loc : locs) {
            if (loc.is_out2) nfa_[loc.state_idx].out2 = target;
            else nfa_[loc.state_idx].out1 = target;
        }
    };

    std::function<NfaFrag(const AstNode*)> build_nfa = [&](const AstNode* node) -> NfaFrag {
        if (!node || node->kind == AstNode::Kind::Empty) {
            NfaState st;
            st.op = NfaState::Op::Split;
            st.out1 = UINT32_MAX;
            st.out2 = UINT32_MAX;
            // Empty epsilon pass-through: use single-branch split (out2 = UINT32_MAX)
            u32 id = emit(st);
            return NfaFrag{id, {{id, false}}};
        }

        switch (node->kind) {
            case AstNode::Kind::Literal: {
                NfaState st;
                if (node->icase && ((node->ch >= 'a' && node->ch <= 'z') || (node->ch >= 'A' && node->ch <= 'Z'))) {
                    st.op = NfaState::Op::LiteralICase;
                    st.ch = ascii_lower(node->ch);
                } else {
                    st.op = NfaState::Op::Literal;
                    st.ch = node->ch;
                }
                u32 id = emit(st);
                return NfaFrag{id, {{id, false}}};
            }
            case AstNode::Kind::Class: {
                NfaState st;
                u8 single = 0;
                if (node->bset.single_char(single)) {
                    st.op = NfaState::Op::Literal;
                    st.ch = single;
                } else {
                    st.op = NfaState::Op::Class;
                    st.class_idx = static_cast<u32>(classes_.size());
                    classes_.push_back(node->bset);
                }
                u32 id = emit(st);
                return NfaFrag{id, {{id, false}}};
            }
            case AstNode::Kind::Anchor: {
                NfaState st;
                switch (node->anchor) {
                    case AstNode::AnchorKind::StartLine: st.op = NfaState::Op::AnchorStart; break;
                    case AstNode::AnchorKind::EndLine: st.op = NfaState::Op::AnchorEnd; break;
                    case AstNode::AnchorKind::WordBoundary: st.op = NfaState::Op::WordBoundary; break;
                    case AstNode::AnchorKind::NonWordBoundary: st.op = NfaState::Op::NonWordBoundary; break;
                }
                u32 id = emit(st);
                return NfaFrag{id, {{id, false}}};
            }
            case AstNode::Kind::Concat: {
                if (node->children.empty()) {
                    return build_nfa(nullptr);
                }
                NfaFrag acc = build_nfa(node->children[0].get());
                for (size_t i = 1; i < node->children.size(); ++i) {
                    NfaFrag next = build_nfa(node->children[i].get());
                    patch(acc.outs, next.start);
                    acc.outs = std::move(next.outs);
                }
                return acc;
            }
            case AstNode::Kind::Alt: {
                if (node->children.empty()) return build_nfa(nullptr);
                NfaFrag acc = build_nfa(node->children[0].get());
                for (size_t i = 1; i < node->children.size(); ++i) {
                    NfaFrag rhs = build_nfa(node->children[i].get());
                    NfaState sp;
                    sp.op = NfaState::Op::Split;
                    sp.out1 = acc.start;
                    sp.out2 = rhs.start;
                    u32 sid = emit(sp);
                    acc.start = sid;
                    acc.outs.insert(acc.outs.end(), rhs.outs.begin(), rhs.outs.end());
                }
                return acc;
            }
            case AstNode::Kind::Repeat: {
                const AstNode* sub = node->children[0].get();
                // Expand min mandatory copies
                std::vector<NfaFrag> parts;
                u32 min_c = std::min<u32>(node->rep_min, 256);
                for (u32 i = 0; i < min_c; ++i) {
                    parts.push_back(build_nfa(sub));
                }
                if (node->rep_max == UINT32_MAX) {
                    // Unbounded tail: (sub)*
                    NfaFrag body = build_nfa(sub);
                    NfaState sp;
                    sp.op = NfaState::Op::Split;
                    if (node->greedy) {
                        sp.out1 = body.start;
                        sp.out2 = UINT32_MAX;
                    } else {
                        sp.out1 = UINT32_MAX;
                        sp.out2 = body.start;
                    }
                    u32 sid = emit(sp);
                    patch(body.outs, sid);
                    PatchLoc exit_loc{sid, node->greedy}; // greedy -> out2 is exit; lazy -> out1 is exit
                    parts.push_back(NfaFrag{sid, {exit_loc}});
                } else {
                    u32 opt_c = std::min<u32>(node->rep_max - node->rep_min, 256);
                    for (u32 i = 0; i < opt_c; ++i) {
                        NfaFrag body = build_nfa(sub);
                        NfaState sp;
                        sp.op = NfaState::Op::Split;
                        if (node->greedy) {
                            sp.out1 = body.start;
                            sp.out2 = UINT32_MAX;
                        } else {
                            sp.out1 = UINT32_MAX;
                            sp.out2 = body.start;
                        }
                        u32 sid = emit(sp);
                        std::vector<PatchLoc> outs = std::move(body.outs);
                        outs.push_back(PatchLoc{sid, node->greedy});
                        parts.push_back(NfaFrag{sid, std::move(outs)});
                    }
                }
                if (parts.empty()) return build_nfa(nullptr);
                NfaFrag acc = std::move(parts[0]);
                for (size_t i = 1; i < parts.size(); ++i) {
                    patch(acc.outs, parts[i].start);
                    acc.outs = std::move(parts[i].outs);
                }
                return acc;
            }
            default:
                return build_nfa(nullptr);
        }
    };

    NfaFrag main_frag = build_nfa(ast.get());
    NfaState match_st;
    match_st.op = NfaState::Op::Match;
    u32 match_id = emit(match_st);
    patch(main_frag.outs, match_id);
    nfa_start_ = main_frag.start;

    // Can we use the fast unanchored Lazy DFA for quick line rejection?
    can_use_dfa_ = !ast_has_anchors(ast.get()) && nfa_.size() <= 512;
    if (can_use_dfa_) {
        // Prepend .*? loop feeding into nfa_start_
        NfaState any_ch;
        any_ch.op = NfaState::Op::Class;
        any_ch.class_idx = static_cast<u32>(classes_.size());
        classes_.push_back(make_dot_set(false));
        u32 any_id = emit(any_ch);

        NfaState loop_sp;
        loop_sp.op = NfaState::Op::Split;
        loop_sp.out1 = nfa_start_;
        loop_sp.out2 = any_id;
        u32 sp_id = emit(loop_sp);
        nfa_[any_id].out1 = sp_id;
        dfa_start_nfa_ = sp_id;
    }

    return true;
}

void CompiledPattern::epsilon_closure(std::vector<u32>& states, std::vector<u8>& visited,
                                      std::string_view line, size_t pos) const {
    size_t idx = 0;
    while (idx < states.size()) {
        u32 s = states[idx++];
        const auto& st = nfa_[s];
        auto push = [&](u32 nxt) {
            if (nxt != UINT32_MAX && !visited[nxt]) {
                visited[nxt] = 1;
                states.push_back(nxt);
            }
        };
        switch (st.op) {
            case NfaState::Op::Split:
                push(st.out1);
                push(st.out2);
                break;
            case NfaState::Op::AnchorStart:
                if (pos == 0) push(st.out1);
                break;
            case NfaState::Op::AnchorEnd:
                if (pos == line.size()) push(st.out1);
                break;
            case NfaState::Op::WordBoundary:
                if (is_word_boundary_at(line, pos)) push(st.out1);
                break;
            case NfaState::Op::NonWordBoundary:
                if (!is_word_boundary_at(line, pos)) push(st.out1);
                break;
            default:
                break;
        }
    }
}

bool CompiledPattern::dfa_has_match(std::string_view line) const {
    if (!can_use_dfa_) return true; // Defer to NFA

    // Lock-free thread-local Lazy DFA cache (0 mutexes, L1-cache-resident transition table!)
    struct LocalDfa {
        u64 pat_id = 0;
        bool overflow = false;
        std::vector<DfaState> states;
        std::vector<std::vector<u32>> state_sets;
        std::unordered_map<std::string, i32> map;
    };
    thread_local LocalDfa cache;

    if (cache.pat_id != pattern_id_) {
        cache.pat_id = pattern_id_;
        cache.overflow = false;
        cache.states.clear();
        cache.state_sets.clear();
        cache.map.clear();
    }
    if (cache.overflow) return true;

    auto get_or_create_state = [&](std::vector<u32>& states) -> i32 {
        std::sort(states.begin(), states.end());
        states.erase(std::unique(states.begin(), states.end()), states.end());
        std::string key(states.size() * sizeof(u32), '\0');
        std::memcpy(key.data(), states.data(), key.size());
        auto it = cache.map.find(key);
        if (it != cache.map.end()) return it->second;
        if (cache.states.size() >= 1024) {
            cache.overflow = true;
            return -2;
        }
        i32 id = static_cast<i32>(cache.states.size());
        DfaState ds;
        for (u32 s : states) {
            if (nfa_[s].op == NfaState::Op::Match) {
                ds.is_match = true;
                break;
            }
        }
        cache.states.push_back(ds);
        cache.state_sets.push_back(std::move(states));
        cache.map.emplace(std::move(key), id);
        return id;
    };

    if (cache.states.empty()) {
        std::vector<u32> init = {dfa_start_nfa_};
        std::vector<u8> vis(nfa_.size(), 0);
        vis[dfa_start_nfa_] = 1;
        epsilon_closure(init, vis, "", 0);
        if (get_or_create_state(init) < 0) return true;
    }

    i32 cur = 0;
    if (cache.states[0].is_match) return true;

    const u8* ptr = reinterpret_cast<const u8*>(line.data());
    const size_t n = line.size();
    for (size_t i = 0; i < n; ++i) {
        u8 c = ptr[i];
        i32 nxt = cache.states[cur].next[c];
        if (nxt >= 0) [[likely]] {
            if (cache.states[nxt].is_match) return true;
            cur = nxt;
            continue;
        }
        std::vector<u32> next_set;
        std::vector<u8> vis(nfa_.size(), 0);
        for (u32 s : cache.state_sets[cur]) {
            const auto& st = nfa_[s];
            bool ok = false;
            if (st.op == NfaState::Op::Literal) ok = (st.ch == c);
            else if (st.op == NfaState::Op::LiteralICase) ok = (ascii_lower(c) == st.ch);
            else if (st.op == NfaState::Op::Class) ok = classes_[st.class_idx].test(c);
            if (ok && st.out1 != UINT32_MAX && !vis[st.out1]) {
                vis[st.out1] = 1;
                next_set.push_back(st.out1);
            }
        }
        epsilon_closure(next_set, vis, "", 0);
        nxt = get_or_create_state(next_set);
        if (nxt < 0) return true;
        cache.states[cur].next[c] = nxt;
        if (cache.states[nxt].is_match) return true;
        cur = nxt;
    }
    return false;
}

// Find leftmost match starting at or after `start_pos` using Thompson NFA.
bool CompiledPattern::nfa_find_first(std::string_view line, size_t start_pos, MatchSpan& out) const {
    const size_t n = line.size();
    const size_t num_states = nfa_.size();
    std::vector<u32> cur_states;
    std::vector<u32> nxt_states;
    std::vector<u8> visited(num_states, 0);

    for (size_t sp = start_pos; sp <= n; ++sp) {
        cur_states.clear();
        std::fill(visited.begin(), visited.end(), 0);
        visited[nfa_start_] = 1;
        cur_states.push_back(nfa_start_);
        epsilon_closure(cur_states, visited, line, sp);

        i64 best_end = -1;
        for (u32 s : cur_states) {
            if (nfa_[s].op == NfaState::Op::Match) {
                best_end = static_cast<i64>(sp);
                break;
            }
        }

        size_t pos = sp;
        while (!cur_states.empty() && pos < n) {
            u8 c = static_cast<u8>(line[pos]);
            nxt_states.clear();
            std::fill(visited.begin(), visited.end(), 0);
            for (u32 s : cur_states) {
                const auto& st = nfa_[s];
                bool ok = false;
                if (st.op == NfaState::Op::Literal) ok = (st.ch == c);
                else if (st.op == NfaState::Op::LiteralICase) ok = (ascii_lower(c) == st.ch);
                else if (st.op == NfaState::Op::Class) ok = classes_[st.class_idx].test(c);
                if (ok && st.out1 != UINT32_MAX && !visited[st.out1]) {
                    visited[st.out1] = 1;
                    nxt_states.push_back(st.out1);
                }
            }
            ++pos;
            if (nxt_states.empty()) break;
            epsilon_closure(nxt_states, visited, line, pos);
            for (u32 s : nxt_states) {
                if (nfa_[s].op == NfaState::Op::Match) {
                    best_end = static_cast<i64>(pos);
                    break;
                }
            }
            cur_states.swap(nxt_states);
        }

        if (best_end >= 0) {
            out.start = static_cast<u32>(sp);
            out.end = static_cast<u32>(best_end);
            return true;
        }
    }
    return false;
}

bool CompiledPattern::match_line_literal(std::string_view line, std::vector<MatchSpan>& out_spans) const {
    out_spans.clear();
    if (literal_.empty()) {
        out_spans.push_back({0, 0});
        return true;
    }
    if (line_regexp_) {
        if (line.size() != literal_.size()) return false;
        bool eq = effective_icase_
            ? simd::memeq_icase_ascii(reinterpret_cast<const u8*>(line.data()),
                                      reinterpret_cast<const u8*>(lower_literal_.data()),
                                      literal_.size())
            : (std::memcmp(line.data(), literal_.data(), literal_.size()) == 0);
        if (!eq) return false;
        if (word_regexp_ && (!is_word_boundary_at(line, 0) || !is_word_boundary_at(line, line.size()))) {
            return false;
        }
        out_spans.push_back({0, static_cast<u32>(line.size())});
        return true;
    }

    const auto* hay = reinterpret_cast<const u8*>(line.data());
    const size_t hay_len = line.size();
    const auto* needle = reinterpret_cast<const u8*>(effective_icase_ ? lower_literal_.data() : literal_.data());
    const size_t nlen = literal_.size();

    size_t pos = 0;
    while (pos + nlen <= hay_len) {
        size_t found = effective_icase_
            ? simd::find_substring_icase(hay + pos, hay_len - pos, needle, nlen)
            : simd::find_substring(hay + pos, hay_len - pos, needle, nlen);
        if (found == SIZE_MAX) break;
        size_t abs_pos = pos + found;
        if (!word_regexp_ ||
            (is_word_boundary_at(line, abs_pos) && is_word_boundary_at(line, abs_pos + nlen))) {
            out_spans.push_back({static_cast<u32>(abs_pos), static_cast<u32>(abs_pos + nlen)});
            pos = abs_pos + nlen;
        } else {
            pos = abs_pos + 1;
        }
    }
    return !out_spans.empty();
}

bool CompiledPattern::match_line_nfa(std::string_view line, std::vector<MatchSpan>& out_spans) const {
    out_spans.clear();

    // Quick SIMD prefilter check on the line
    if (prefilter_.size() >= 2) {
        size_t f = prefilter_icase_
            ? simd::find_substring_icase(reinterpret_cast<const u8*>(line.data()), line.size(),
                                         reinterpret_cast<const u8*>(prefilter_.data()), prefilter_.size())
            : simd::find_substring(reinterpret_cast<const u8*>(line.data()), line.size(),
                                   reinterpret_cast<const u8*>(prefilter_.data()), prefilter_.size());
        if (f == SIZE_MAX) return false;
    }

    // Fast Lazy DFA rejection for anchor-free patterns
    if (can_use_dfa_ && !dfa_has_match(line)) {
        return false;
    }

    size_t pos = 0;
    while (pos <= line.size()) {
        MatchSpan sp;
        if (!nfa_find_first(line, pos, sp)) break;
        out_spans.push_back(sp);
        pos = (sp.end > sp.start) ? sp.end : (sp.start + 1);
    }
    return !out_spans.empty();
}

bool CompiledPattern::match_line(std::string_view line, std::vector<MatchSpan>& out_spans) const {
    if (is_pure_literal_) {
        return match_line_literal(line, out_spans);
    }
    return match_line_nfa(line, out_spans);
}

void CompiledPattern::search_buffer(
    const u8* data, size_t len,
    const std::function<bool(u32 line_no, std::string_view line,
                             const std::vector<MatchSpan>& spans)>& cb,
    const std::atomic<bool>* cancel) const {

    if (len == 0) return;
    std::vector<MatchSpan> spans;
    spans.reserve(8);

    // Ultra-fast buffer-wide SIMD literal jump!
    // Instead of splitting every line in a 100KB file, jump directly to candidate bytes
    // with AVX-512 find_substring, count '\n' via SIMD popcount, and verify only hit lines!
    const bool use_buf_prefilter =
        (is_pure_literal_ && !literal_.empty()) || (!is_pure_literal_ && prefilter_.size() >= 2);

    if (use_buf_prefilter) {
        const auto* needle = reinterpret_cast<const u8*>(
            is_pure_literal_
                ? (effective_icase_ ? lower_literal_.data() : literal_.data())
                : prefilter_.data());
        const size_t nlen = is_pure_literal_ ? literal_.size() : prefilter_.size();
        const bool ic = is_pure_literal_ ? effective_icase_ : prefilter_icase_;

        size_t search_pos = 0;
        size_t counted_pos = 0;
        u32 line_no = 1;

        while (search_pos < len) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return;

            size_t rel = ic
                ? simd::find_substring_icase(data + search_pos, len - search_pos, needle, nlen)
                : simd::find_substring(data + search_pos, len - search_pos, needle, nlen);
            if (rel == SIZE_MAX) return;

            size_t hit = search_pos + rel;

            // Find enclosing line [line_start, line_end)
            size_t prev_nl = simd::rfind_byte(data, hit, '\n');
            size_t line_start = (prev_nl == SIZE_MAX) ? 0 : (prev_nl + 1);
            size_t line_end = simd::find_byte(data + hit, len - hit, '\n');
            line_end = hit + line_end;

            // Update 1-based line number using AVX-512 popcount of '\n'
            if (line_start > counted_pos) {
                line_no += static_cast<u32>(simd::count_byte(data + counted_pos, line_start - counted_pos, '\n'));
                counted_pos = line_start;
            }

            size_t trimmed_end = line_end;
            if (trimmed_end > line_start && data[trimmed_end - 1] == '\r') {
                --trimmed_end;
            }
            std::string_view line_sv(reinterpret_cast<const char*>(data + line_start),
                                     trimmed_end - line_start);

            if (match_line(line_sv, spans)) {
                if (!cb(line_no, line_sv, spans)) return;
            }

            search_pos = (line_end < len) ? (line_end + 1) : len;
        }
        return;
    }

    // General line-by-line scan (e.g. `.*`, `^$`, `[0-9]{4}`)
    size_t pos = 0;
    u32 line_no = 1;
    while (pos < len) {
        if (cancel && (line_no & 63) == 0 && cancel->load(std::memory_order_relaxed)) return;

        size_t nl = simd::find_byte(data + pos, len - pos, '\n');
        size_t line_end = pos + nl;
        size_t trimmed_end = line_end;
        if (trimmed_end > pos && data[trimmed_end - 1] == '\r') {
            --trimmed_end;
        }
        std::string_view line_sv(reinterpret_cast<const char*>(data + pos), trimmed_end - pos);
        if (match_line(line_sv, spans)) {
            if (!cb(line_no, line_sv, spans)) return;
        }
        pos = (line_end < len) ? (line_end + 1) : len;
        ++line_no;
    }
}

} // namespace scry
