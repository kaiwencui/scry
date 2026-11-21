#pragma once

#include "common.hpp"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scry {

// Boolean Trigram Query produced from a Regex AST or literal pattern
struct TrigramQuery {
    enum class Kind : u8 {
        All,  // Matches any document (no trigram restriction)
        None, // Matches no document
        Atom, // Single trigram (exact or lowercased)
        And,  // All children must match
        Or    // Any child must match
    };

    Kind kind = Kind::All;
    Trigram trigram = 0;
    bool icase = false;
    std::vector<TrigramQuery> children;

    static TrigramQuery make_all() { return TrigramQuery{Kind::All, 0, false, {}}; }
    static TrigramQuery make_none() { return TrigramQuery{Kind::None, 0, false, {}}; }
    static TrigramQuery make_atom(Trigram t, bool ic) {
        return TrigramQuery{Kind::Atom, ic ? lower_trigram(t) : t, ic, {}};
    }
    static TrigramQuery make_and(std::vector<TrigramQuery> ch);
    static TrigramQuery make_or(std::vector<TrigramQuery> ch);

    std::string to_string() const;
};

struct MatchSpan {
    u32 start = 0; // Byte offset within line
    u32 end = 0;   // Exclusive byte offset within line
};

struct RegexOptions {
    bool case_insensitive = false;
    bool smart_case = false;
    bool fixed_strings = false;
    bool word_regexp = false;
    bool line_regexp = false;
};

// 256-bit ASCII/byte character class
struct ByteSet {
    std::array<u64, 4> bits = {0, 0, 0, 0};

    void set(u8 c) noexcept { bits[c >> 6] |= (1ULL << (c & 63)); }
    void clear(u8 c) noexcept { bits[c >> 6] &= ~(1ULL << (c & 63)); }
    bool test(u8 c) const noexcept { return (bits[c >> 6] & (1ULL << (c & 63))) != 0; }
    void invert() noexcept {
        for (auto& w : bits) w = ~w;
    }
    void merge(const ByteSet& o) noexcept {
        for (int i = 0; i < 4; ++i) bits[i] |= o.bits[i];
    }
    int popcount() const noexcept;
    bool single_char(u8& out) const noexcept;
};

// Compiled Regex + Trigram Query + SIMD Prefilter + Thompson NFA / Lazy DFA
class CompiledPattern {
public:
    CompiledPattern() = default;

    // Compile pattern; returns false and fills `err` on syntax error
    bool compile(std::string_view pattern, const RegexOptions& opts, std::string& err);

    const TrigramQuery& trigram_query() const noexcept { return trigram_query_; }
    bool is_pure_literal() const noexcept { return is_pure_literal_; }
    bool effective_icase() const noexcept { return effective_icase_; }
    const std::string& literal() const noexcept { return literal_; }
    const std::string& lower_literal() const noexcept { return lower_literal_; }
    const std::string& prefilter() const noexcept { return prefilter_; }
    bool prefilter_icase() const noexcept { return prefilter_icase_; }

    // Fast buffer-level search: invokes callback(line_no_1based, line_sv, spans) for each matching line.
    // Supports early-stop by returning false from callback.
    // Optional atomic cancellation check.
    using LineCallback = bool(*)(void* ctx, u32 line_no, std::string_view line,
                                 const std::vector<MatchSpan>& spans);

    void search_buffer(const u8* data, size_t len,
                       const std::function<bool(u32 line_no, std::string_view line,
                                                const std::vector<MatchSpan>& spans)>& cb,
                       const std::atomic<bool>* cancel = nullptr) const;

    // Match a single line (no trailing '\n'); returns true if matched and populates spans.
    bool match_line(std::string_view line, std::vector<MatchSpan>& out_spans) const;

private:
    struct NfaState {
        enum class Op : u8 {
            Match,
            Split,
            Literal,
            LiteralICase,
            Class,
            AnchorStart,
            AnchorEnd,
            WordBoundary,
            NonWordBoundary
        };
        Op op = Op::Match;
        u8 ch = 0;
        u32 out1 = 0;
        u32 out2 = 0;
        u32 class_idx = 0;
    };

    // Lazy DFA state for anchor-free fast line matching
    struct DfaState {
        std::array<i32, 256> next;
        bool is_match = false;
        DfaState() { next.fill(-1); }
    };

    bool match_line_literal(std::string_view line, std::vector<MatchSpan>& out_spans) const;
    bool match_line_nfa(std::string_view line, std::vector<MatchSpan>& out_spans) const;
    bool dfa_has_match(std::string_view line) const;
    bool nfa_find_first(std::string_view line, size_t start_pos, MatchSpan& out) const;
    void epsilon_closure(std::vector<u32>& states, std::vector<u8>& visited,
                         std::string_view line, size_t pos) const;

    bool effective_icase_ = false;
    bool is_pure_literal_ = false;
    bool word_regexp_ = false;
    bool line_regexp_ = false;
    bool can_use_dfa_ = false;

    std::string literal_;
    std::string lower_literal_;
    std::string prefilter_;
    bool prefilter_icase_ = false;

    TrigramQuery trigram_query_;

    std::vector<NfaState> nfa_;
    std::vector<ByteSet> classes_;
    u32 nfa_start_ = 0;
    u32 dfa_start_nfa_ = 0; // Unanchored .*? + pattern NFA start
    u64 pattern_id_ = 0;    // Unique ID for lock-free thread_local Lazy DFA cache
};

} // namespace scry
