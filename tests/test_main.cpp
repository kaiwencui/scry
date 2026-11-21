#include "cli.hpp"
#include "daemon.hpp"
#include "delta.hpp"
#include "gitignore.hpp"
#include "indexer.hpp"
#include "io_uring_reader.hpp"
#include "posting.hpp"
#include "regex_engine.hpp"
#include "searcher.hpp"
#include "simd.hpp"
#include "symbols.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace scry;

namespace {

int g_passed = 0;
int g_failed = 0;

#define EXPECT(cond)                                                                           \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failed;                                                                        \
            return;                                                                            \
        }                                                                                      \
    } while (0)

struct TempDir {
    std::filesystem::path path;
    TempDir(const char* prefix) {
        auto base = std::filesystem::temp_directory_path();
        auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / (std::string(prefix) + "_" + std::to_string(::getpid()) + "_" + std::to_string(ts));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write_file(const std::string& rel, const std::string& content) const {
        auto full = path / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
};

void test_simd() {
    std::string s(1024, 'a');
    s[777] = '\0';
    EXPECT(simd::contains_nul(reinterpret_cast<const u8*>(s.data()), s.size()));
    s[777] = 'b';
    EXPECT(!simd::contains_nul(reinterpret_cast<const u8*>(s.data()), s.size()));

    std::string hay = std::string(150, 'x') + "sched_entity_handler" + std::string(200, 'y');
    size_t pos = simd::find_substring(reinterpret_cast<const u8*>(hay.data()), hay.size(),
                                      reinterpret_cast<const u8*>("sched_entity"), 12);
    EXPECT(pos == 150);

    std::string hay_up = std::string(150, 'x') + "ScHeD_EnTiTy_handler" + std::string(200, 'y');
    size_t pos_ic = simd::find_substring_icase(reinterpret_cast<const u8*>(hay_up.data()), hay_up.size(),
                                               reinterpret_cast<const u8*>("sched_entity"), 12);
    EXPECT(pos_ic == 150);

    std::string lines = "line1\nline2\nline3\nline4\n";
    EXPECT(simd::count_byte(reinterpret_cast<const u8*>(lines.data()), lines.size(), '\n') == 4);
    EXPECT(simd::rfind_byte(reinterpret_cast<const u8*>(lines.data()), 10, '\n') == 5);

    // Sorted u32 intersection
    std::vector<u32> a, b, out;
    for (u32 i = 0; i < 1000; i += 2) a.push_back(i);
    for (u32 i = 0; i < 1000; i += 3) b.push_back(i);
    simd::intersect_sorted_u32(a.data(), a.size(), b.data(), b.size(), out);
    EXPECT(out.size() == 167); // multiples of 6 in [0..999]: 0, 6, ..., 996
    EXPECT(out.front() == 0 && out.back() == 996);

    // BP128 roundtrip
    alignas(64) u32 deltas[128];
    u32 expected[128];
    u32 acc = 1000;
    deltas[0] = 0;
    expected[0] = acc;
    for (int i = 1; i < 128; ++i) {
        deltas[i] = static_cast<u32>((i * 7) % 50 + 1);
        acc += deltas[i];
        expected[i] = acc;
    }
    u8 bw = simd::max_bit_width_128(deltas);
    EXPECT(bw == 6);
    std::vector<u8> packed(static_cast<size_t>(bw) * 16);
    simd::bp128_pack(deltas, bw, packed.data());
    alignas(64) u32 unpacked[128];
    simd::bp128_unpack_deltas(packed.data(), bw, expected[0], unpacked);
    for (int i = 0; i < 128; ++i) EXPECT(unpacked[i] == expected[i]);

    ++g_passed;
}

void test_posting_lists() {
    std::vector<u8> arena;
    const u32 total_docs = 5000;

    // Tier 0: Inline (<= 2 docs)
    std::vector<DocId> d_inline = {42, 1337};
    auto e_inline = encode_posting_list(pack_trigram('f', 'o', 'o'), d_inline, total_docs, arena);
    EXPECT(e_inline.encoding() == PostingEncoding::Inline);
    std::vector<DocId> dec;
    decode_posting_list(e_inline, arena.data(), total_docs, dec);
    EXPECT(dec == d_inline);

    // Tier 1: BP128 (300 docs = 2 blocks of 128 + 44 varint tail)
    std::vector<DocId> d_bp;
    for (u32 i = 0; i < 300; ++i) d_bp.push_back(i * 15 + 3);
    auto e_bp = encode_posting_list(pack_trigram('b', 'a', 'r'), d_bp, total_docs, arena);
    EXPECT(e_bp.encoding() == PostingEncoding::BP128);
    decode_posting_list(e_bp, arena.data(), total_docs, dec);
    EXPECT(dec == d_bp);

    // Tier 2: Dense Bitset (>= total_docs / 16)
    std::vector<DocId> d_dense;
    for (u32 i = 0; i < total_docs; i += 5) d_dense.push_back(i);
    auto e_dense = encode_posting_list(pack_trigram('i', 'n', 't'), d_dense, total_docs, arena);
    EXPECT(e_dense.encoding() == PostingEncoding::Bitset);
    decode_posting_list(e_dense, arena.data(), total_docs, dec);
    EXPECT(dec == d_dense);

    // Block-skipping BP128 intersection + Bitset intersection
    std::vector<DocId> cand = {3, 18, 20, 1503, 4488, 4999};
    intersect_candidates_with_posting(cand, e_bp, arena.data(), total_docs);
    // 3 = 0*15+3, 18 = 1*15+3, 1503 = 100*15+3, 4488 = 299*15+3
    std::vector<DocId> exp1 = {3, 18, 1503, 4488};
    EXPECT(cand == exp1);

    // Now intersect with dense multiples of 5 -> none of {3, 18, 1503, 4488} are divisible by 5!
    std::vector<DocId> cand2 = {0, 3, 15, 18, 500, 1503};
    intersect_candidates_with_posting(cand2, e_dense, arena.data(), total_docs);
    std::vector<DocId> exp2 = {0, 15, 500};
    EXPECT(cand2 == exp2);

    ++g_passed;
}

void test_regex_and_planner() {
    std::string err;
    CompiledPattern p;

    // Pure literal
    EXPECT(p.compile("sched_entity", {}, err));
    EXPECT(p.is_pure_literal());
    EXPECT(p.trigram_query().kind == TrigramQuery::Kind::And);
    std::vector<MatchSpan> spans;
    EXPECT(p.match_line("struct sched_entity *se;", spans));
    EXPECT(spans.size() == 1 && spans[0].start == 7 && spans[0].end == 19);

    // Regex alternation + suffix -> exact trigram OR/AND
    EXPECT(p.compile("(foo|bar)_handler", {}, err));
    EXPECT(!p.is_pure_literal());
    EXPECT(p.trigram_query().kind != TrigramQuery::Kind::All);
    EXPECT(p.match_line("void bar_handler(int x)", spans));
    EXPECT(spans.size() == 1 && spans[0].start == 5 && spans[0].end == 16);
    EXPECT(!p.match_line("void baz_handler(int x)", spans));

    // Anchors, word boundaries, quantifiers, case-insensitive
    RegexOptions ro;
    ro.case_insensitive = true;
    EXPECT(p.compile("^\\s*fn\\s+[a-z0-9_]+\\(", ro, err));
    EXPECT(p.match_line("  FN my_Func_1(", spans));
    EXPECT(!p.match_line("let x = fn_call(", spans));

    // Word regexp (-w)
    RegexOptions wr;
    wr.word_regexp = true;
    EXPECT(p.compile("foo", wr, err));
    EXPECT(p.match_line("int foo = 1;", spans));
    EXPECT(!p.match_line("int foobar = 1;", spans));

    // Smart case (-S)
    RegexOptions sc;
    sc.smart_case = true;
    EXPECT(p.compile("sched", sc, err));
    EXPECT(p.effective_icase());
    EXPECT(p.match_line("SCHED_FAIR", spans));
    EXPECT(p.compile("Sched", sc, err));
    EXPECT(!p.effective_icase());
    EXPECT(!p.match_line("sched_fair", spans));

    ++g_passed;
}

void test_gitignore_and_globs() {
    EXPECT(glob_match("*.hpp", "searcher.hpp"));
    EXPECT(!glob_match("*.hpp", "searcher.cpp"));
    EXPECT(glob_match("src/**/*.cpp", "src/core/searcher.cpp"));
    EXPECT(glob_match("src/**/*.cpp", "src/searcher.cpp"));

    GlobFilter gf;
    gf.add("*.cpp");
    gf.add("!test_*.cpp");
    EXPECT(gf.matches("src/main.cpp"));
    EXPECT(!gf.matches("tests/test_main.cpp"));
    EXPECT(!gf.matches("src/main.hpp"));

    GitignoreMatcher gi;
    gi.add_pattern("*.o", "");
    gi.add_pattern("build/", "");
    gi.add_pattern("!build/keep.txt", "");
    gi.add_pattern("/root_only.log", "");
    EXPECT(gi.is_ignored("foo/bar.o", false));
    EXPECT(gi.is_ignored("build", true));
    EXPECT(!gi.is_ignored("build", false));
    EXPECT(gi.is_ignored("root_only.log", false));
    EXPECT(!gi.is_ignored("sub/root_only.log", false));

    ++g_passed;
}

void test_index_search_and_delta() {
    TempDir tmp("scry_test_repo");
    tmp.write_file(".gitignore", "ignored_dir/\n*.log\n");
    tmp.write_file("ignored_dir/secret.cpp", "int sched_entity_ignored = 1;\n");
    tmp.write_file("debug.log", "sched_entity in log\n");
    tmp.write_file("src/sched.cpp",
                   "#include <cstdio>\n"
                   "struct SchedEntity {\n"
                   "    int vruntime;\n"
                   "};\n"
                   "void update_curr(SchedEntity* se) {\n"
                   "    se->vruntime += 10;\n"
                   "}\n");
    tmp.write_file("src/main.rs",
                   "pub struct FastIndex {\n"
                   "    docs: u32,\n"
                   "}\n"
                   "pub fn query_trigrams(q: &str) -> usize {\n"
                   "    q.len()\n"
                   "}\n");
    tmp.write_file("bin.dat", std::string("sched_entity\0binary\0data", 24));

    // Build index
    IndexBuildOptions bopts;
    bopts.output_path = tmp.path / ".scry" / "index.scry";
    IndexBuildStats bstats;
    std::string err;
    EXPECT(build_index(tmp.path, bopts, bstats, err));
    EXPECT(bstats.files_indexed == 2); // Only src/sched.cpp and src/main.rs!
    EXPECT(bstats.files_skipped_binary == 1);

    // Search exact literal via MmapIndex
    SearchOptions sopts;
    sopts.pattern = "SchedEntity";
    sopts.target_path = tmp.path;
    sopts.use_daemon = false;
    auto resp = run_search(sopts);
    EXPECT(resp.error.empty());
    EXPECT(resp.stats.used_index);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].rel_path == "src/sched.cpp");
    EXPECT(resp.files[0].match_count == 2);

    // Case-insensitive search using hybrid lowercased trigram table
    sopts.pattern = "schedentity";
    sopts.regex_opts.case_insensitive = true;
    resp = run_search(sopts);
    EXPECT(resp.stats.used_index);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].match_count == 2);

    // Type filter (-t rust)
    SearchOptions topts;
    topts.pattern = "struct";
    topts.target_path = tmp.path;
    topts.include_types = {LangId::Rust};
    topts.use_daemon = false;
    resp = run_search(topts);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].rel_path == "src/main.rs");

    // Symbol definition search (--def)
    SearchOptions dopts;
    dopts.pattern = "query_trigrams";
    dopts.target_path = tmp.path;
    dopts.def_only = true;
    dopts.use_daemon = false;
    resp = run_search(dopts);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].matches[0].line_no == 4);
    EXPECT(resp.files[0].matches[0].is_definition);

    // Test LSM Delta Overlay (edit + add + delete + concurrent readers!)
    LiveIndex live;
    EXPECT(live.load(tmp.path, bopts.output_path));

    // Modify src/sched.cpp, add src/new_file.cpp, delete src/main.rs
    tmp.write_file("src/sched.cpp", "struct SchedEntity {\n    int updated_field_xyz;\n};\n");
    tmp.write_file("src/new_file.cpp", "int brand_new_token_999() { return 42; }\n");
    std::filesystem::remove(tmp.path / "src/main.rs");

    auto ust = live.apply_batch({"src/sched.cpp", "src/new_file.cpp", "src/main.rs"});
    EXPECT(ust.updated == 2);
    EXPECT(ust.deleted == 1);

    auto snap = live.snapshot();
    SearchOptions q1;
    q1.pattern = "brand_new_token_999";
    auto r1 = search_snapshot(*snap, q1);
    EXPECT(r1.files.size() == 1 && r1.files[0].rel_path == "src/new_file.cpp");

    // Old deleted file must NEVER return stale results!
    SearchOptions q2;
    q2.pattern = "FastIndex";
    auto r2 = search_snapshot(*snap, q2);
    EXPECT(r2.files.empty());

    // Old deleted line in modified file must NEVER return stale results!
    SearchOptions q3;
    q3.pattern = "update_curr";
    auto r3 = search_snapshot(*snap, q3);
    EXPECT(r3.files.empty());

    // New line in modified file MUST be searchable immediately!
    SearchOptions q4;
    q4.pattern = "updated_field_xyz";
    auto r4 = search_snapshot(*snap, q4);
    EXPECT(r4.files.size() == 1 && r4.files[0].rel_path == "src/sched.cpp");

    // Daemon-less --fresh check on modified repo
    SearchOptions q5;
    q5.pattern = "brand_new_token_999";
    q5.target_path = tmp.path;
    q5.strict_freshness = true;
    q5.use_daemon = false;
    auto r5 = run_search(q5);
    EXPECT(r5.files.size() == 1 && r5.files[0].rel_path == "src/new_file.cpp");

    ++g_passed;
}

void test_daemon_and_inotify() {
    TempDir tmp("scry_test_daemon");
    tmp.write_file("alpha.cpp", "int initial_marker_alpha = 1;\n");

    auto sock = tmp.path / "test_scryd.sock";
    ScryDaemon daemon(sock);
    std::string err;
    EXPECT(daemon.watch_repo(tmp.path, err));

    std::thread srv([&]() { daemon.run(); });

    // Wait for socket to come up
    bool up = false;
    for (int i = 0; i < 50; ++i) {
        if (daemon_ping(sock)) { up = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT(up);

    // Query initial file via Unix domain socket IPC
    SearchOptions opts;
    opts.pattern = "initial_marker_alpha";
    opts.target_path = tmp.path;
    SearchResponse resp;
    EXPECT(daemon_query(opts, resp, sock));
    EXPECT(resp.stats.used_daemon);
    EXPECT(resp.files.size() == 1 && resp.files[0].rel_path == "alpha.cpp");

    // Live edit a new file on disk -> inotify updates delta overlay automatically!
    tmp.write_file("beta.cpp", "void live_inotify_magic_symbol() {}\n");

    bool found_live = false;
    for (int i = 0; i < 40; ++i) {
        SearchOptions q2;
        q2.pattern = "live_inotify_magic_symbol";
        q2.target_path = tmp.path;
        SearchResponse r2;
        if (daemon_query(q2, r2, sock) && r2.files.size() == 1) {
            found_live = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    EXPECT(found_live);

    std::string status_txt;
    EXPECT(daemon_status(status_txt, sock));
    EXPECT(status_txt.find("scryd running") != std::string::npos);

    EXPECT(daemon_stop(sock));
    srv.join();

    ++g_passed;
}

void test_json_and_context_formatting() {
    TempDir tmp("scry_test_fmt");
    tmp.write_file("sample.cpp", "line1\nline2\nMATCH_ME\nline4\nline5\n");

    SearchOptions opts;
    opts.pattern = "MATCH_ME";
    opts.target_path = tmp.path;
    opts.before_context = 1;
    opts.after_context = 1;
    opts.force_no_index = true;
    opts.use_daemon = false;

    auto resp = run_search(opts);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].matches[0].before.size() == 1);
    EXPECT(resp.files[0].matches[0].after.size() == 1);

    std::ostringstream oss;
    opts.json_output = true;
    format_search_response(resp, opts, false, oss);
    std::string js = oss.str();
    EXPECT(js.find("\"type\":\"match\"") != std::string::npos);
    EXPECT(js.find("\"line_number\":3") != std::string::npos);

    ++g_passed;
}

void test_large_io_uring_indexing() {
    TempDir tmp("scry_test_uring_1500");
    for (int i = 0; i < 1500; ++i) {
        std::string content = "struct SingleLineStruct_" + std::to_string(i) + " { int v; };\n";
        if (i == 1499) content += "void last_file_unique_sentinel_1499() {}\n";
        tmp.write_file("pkg_" + std::to_string(i % 10) + "/file_" + std::to_string(i) + ".cpp", content);
    }

    IndexBuildOptions bopts;
    bopts.output_path = tmp.path / ".scry" / "index.scry";
    bopts.use_io_uring = true;
    IndexBuildStats bstats;
    std::string err;
    EXPECT(build_index(tmp.path, bopts, bstats, err));
    EXPECT(bstats.files_indexed == 1500);

    // Verify last file is searchable AND single-line struct definition was indexed!
    SearchOptions sopts;
    sopts.pattern = "last_file_unique_sentinel_1499";
    sopts.target_path = tmp.path;
    sopts.use_daemon = false;
    auto resp = run_search(sopts);
    EXPECT(resp.files.size() == 1);

    SearchOptions dopts;
    dopts.pattern = "SingleLineStruct_1499";
    dopts.target_path = tmp.path;
    dopts.def_only = true;
    dopts.use_daemon = false;
    auto dresp = run_search(dopts);
    EXPECT(dresp.files.size() == 1);
    EXPECT(dresp.files[0].matches[0].is_definition);

    ++g_passed;
}

void test_multi_chunk_large_file() {
    TempDir tmp("scry_test_chunks");
    // Build a ~150 KB file (~3000 lines, spanning 5 virtual 32 KiB chunks)
    std::string big;
    big.reserve(160000);
    for (int ln = 1; ln <= 3000; ++ln) {
        if (ln == 10) big += "int NEEDLE_ALPHA_BETA_GAMMA = 10;\n";
        else if (ln == 1500) big += "int NEEDLE_ALPHA_BETA_GAMMA = 1500;\n";
        else if (ln == 2990) big += "int NEEDLE_ALPHA_BETA_GAMMA = 2990;\n";
        else if (ln == 500) big += "int ONLY_PREFIX_PART_ONE = 500;\n";
        else if (ln == 2500) big += "int ONLY_SUFFIX_PART_TWO = 2500;\n";
        else big += "// filler padding line number " + std::to_string(ln) + " with boilerplate code...\n";
    }
    tmp.write_file("huge_header.h", big);

    IndexBuildOptions bopts;
    bopts.output_path = tmp.path / ".scry" / "index.scry";
    IndexBuildStats bstats;
    std::string err;
    EXPECT(build_index(tmp.path, bopts, bstats, err));

    MmapIndex idx;
    EXPECT(idx.open(bopts.output_path));
    EXPECT(idx.num_docs() == 1);
    EXPECT(idx.num_chunks() >= 4);

    // Query hitting chunks 0, 2, 4 -> MUST report exact global line numbers 10, 1500, 2990!
    SearchOptions sopts;
    sopts.pattern = "NEEDLE_ALPHA_BETA_GAMMA";
    sopts.target_path = tmp.path;
    sopts.use_daemon = false;
    auto resp = run_search(sopts);
    EXPECT(resp.files.size() == 1);
    EXPECT(resp.files[0].matches.size() == 3);
    EXPECT(resp.files[0].matches[0].line_no == 10);
    EXPECT(resp.files[0].matches[1].line_no == 1500);
    EXPECT(resp.files[0].matches[2].line_no == 2990);

    // Cross-chunk false-positive elimination: ONLY_PREFIX_PART_ONE is in chunk 1,
    // ONLY_SUFFIX_PART_TWO is in chunk 4 -> querying "ONLY_PREFIX_PART_ONE_ONLY_SUFFIX" has 0 candidates!
    SearchOptions fp_opts;
    fp_opts.pattern = "PREFIX_PART_ONE.*SUFFIX_PART_TWO";
    fp_opts.target_path = tmp.path;
    fp_opts.use_daemon = false;
    auto fp_resp = run_search(fp_opts);
    EXPECT(fp_resp.stats.candidate_docs == 0);
    EXPECT(fp_resp.files.empty());

    ++g_passed;
}

void test_last_mile_ergonomics() {
    // 1. Stdin buffer search + --vimgrep formatting (multiple matches per line)
    {
        std::string input = "alpha foo beta foo gamma\nno match here\nFOO third\n";
        SearchOptions opts;
        opts.pattern = "foo";
        opts.regex_opts.case_insensitive = true;
        opts.vimgrep = true;
        opts.show_line_numbers = true;
        opts.show_column = true;
        opts.with_filename = true;
        auto resp = search_stdin_buffer(reinterpret_cast<const u8*>(input.data()), input.size(), opts);
        EXPECT(resp.files.size() == 1);
        EXPECT(resp.files[0].matches.size() == 2);
        EXPECT(resp.files[0].matches[0].spans.size() == 2);

        std::ostringstream oss;
        format_search_response(resp, opts, false, oss);
        std::string out = oss.str();
        // Line 1 col 7, line 1 col 16, line 3 col 1
        EXPECT(out.find("<stdin>:1:7:") != std::string::npos);
        EXPECT(out.find("<stdin>:1:16:") != std::string::npos);
        EXPECT(out.find("<stdin>:3:1:") != std::string::npos);

        // -q quiet outputs nothing
        opts.quiet = true;
        std::ostringstream qoss;
        format_search_response(resp, opts, false, qoss);
        EXPECT(qoss.str().empty());
    }

    // 2. Multi-pattern (-e) & multi-path CLI parsing + execution
    {
        TempDir tmp("scry_test_multi");
        tmp.write_file("dir_a/one.cpp", "int alpha_needle_111 = 1;\n");
        tmp.write_file("dir_b/two.cpp", "int beta_needle_222 = 2;\n");

        std::string p1 = (tmp.path / "dir_a").string();
        std::string p2 = (tmp.path / "dir_b").string();
        char* argv[] = {
            const_cast<char*>("scry"),
            const_cast<char*>("-e"), const_cast<char*>("alpha_needle_111"),
            const_cast<char*>("-e"), const_cast<char*>("beta_needle_222"),
            const_cast<char*>("--no-daemon"),
            const_cast<char*>(p1.c_str()),
            const_cast<char*>(p2.c_str())
        };
        auto parsed = parse_cli(8, argv);
        EXPECT(parsed.error.empty());
        EXPECT(parsed.search_opts.target_paths.size() == 2);
        auto resp = run_search(parsed.search_opts);
        EXPECT(resp.files.size() == 2);
    }

    // 3. Automatic unstaged-edit freshness on small/medium repo WITHOUT scryd or --fresh!
    {
        TempDir tmp("scry_test_autofresh");
        tmp.write_file("a.cpp", "int original_token_xyz = 1;\n");
        IndexBuildOptions bopts;
        bopts.output_path = tmp.path / ".scry" / "index.scry";
        IndexBuildStats bstats;
        std::string err;
        EXPECT(build_index(tmp.path, bopts, bstats, err));

        // Edit file on disk WITHOUT running scry index, WITHOUT scryd, WITHOUT --fresh!
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        tmp.write_file("a.cpp", "int brand_new_unstaged_edit_98765 = 2;\n");

        SearchOptions q;
        q.pattern = "brand_new_unstaged_edit_98765";
        q.target_path = tmp.path;
        q.use_daemon = false;
        q.strict_freshness = false;
        auto r = run_search(q);
        EXPECT(r.files.size() == 1);
        EXPECT(r.files[0].rel_path == "a.cpp");

        // Old deleted token must NOT match either
        q.pattern = "original_token_xyz";
        auto r_old = run_search(q);
        EXPECT(r_old.files.empty());
    }

    // 4. Persisted watched repos config + unwatch
    {
        TempDir cfg("scry_test_cfg");
        TempDir repo("scry_test_wrepo");
        ::setenv("SCRY_CONFIG_DIR", cfg.path.c_str(), 1);
        EXPECT(persist_watched_repo(repo.path));
        auto list = load_persisted_repos();
        EXPECT(list.size() == 1);
        EXPECT(unpersist_watched_repo(repo.path));
        EXPECT(load_persisted_repos().empty());
        ::unsetenv("SCRY_CONFIG_DIR");
    }

    ++g_passed;
}

} // namespace

int main() {
    std::printf("Running scry unit & integration tests (SIMD=%s)...\n", simd::arch_name());
    test_simd();
    test_posting_lists();
    test_regex_and_planner();
    test_gitignore_and_globs();
    test_index_search_and_delta();
    test_daemon_and_inotify();
    test_json_and_context_formatting();
    test_large_io_uring_indexing();
    test_multi_chunk_large_file();
    test_last_mile_ergonomics();

    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
