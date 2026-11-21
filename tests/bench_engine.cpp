#include "delta.hpp"
#include "indexer.hpp"
#include "searcher.hpp"
#include "simd.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace scry;

namespace {

// Deterministic realistic multi-language code generator for reproducible benchmarks
void generate_synthetic_corpus(const std::filesystem::path& dir, u32 num_files, u32 lines_per_file) {
    std::filesystem::create_directories(dir);
    static const char* kModules[] = {
        "kernel/sched", "kernel/mm", "drivers/net", "drivers/gpu",
        "llvm/IR", "llvm/Transforms", "clang/Sema", "clang/AST",
        "runtime/gc", "engine/index", "engine/query", "ui/tui"
    };
    static const char* kExts[] = {".cpp", ".hpp", ".c", ".h", ".rs", ".py", ".go"};

    for (u32 f = 0; f < num_files; ++f) {
        const char* mod = kModules[f % 12];
        const char* ext = kExts[f % 7];
        auto sub = dir / mod;
        std::filesystem::create_directories(sub);
        auto fp = sub / ("unit_" + std::to_string(f) + ext);

        std::string content;
        content.reserve(lines_per_file * 56);
        content += "// Auto-generated realistic benchmark source file " + std::to_string(f) + "\n";
        content += "#include <stdint.h>\n";
        content += "struct ModuleContext_" + std::to_string(f) + " {\n";
        content += "    uint64_t generation_id;\n";
        content += "    uint32_t active_workers;\n";
        content += "};\n\n";
        content += "int process_packet_" + std::to_string(f) + "(int flags, const char* buf) {\n";

        for (u32 l = 0; l < lines_per_file; ++l) {
            u32 h = (f * 2654435761u) ^ (l * 2246822519u);
            switch (h % 8) {
                case 0:
                    content += "    if (flags & 0x04) { return compute_delta_offset(flags, " +
                               std::to_string(l) + "); }\n";
                    break;
                case 1:
                    content += "    uint64_t hash_val = mix_trigram_bits(buf, " + std::to_string(h & 255) + ");\n";
                    break;
                case 2:
                    content += "    for (int i = 0; i < 16; ++i) { acc += table_lookup[i]; }\n";
                    break;
                case 3:
                    content += "    // Track scheduling latency and cache locality across CCDs\n";
                    break;
                case 4:
                    content += "    auto result = execute_pipeline_stage(ctx, hash_val);\n";
                    break;
                case 5:
                    content += "    if (result == nullptr) return -1;\n";
                    break;
                case 6:
                    content += "    metrics_record_latency_ns(result->elapsed, " + std::to_string(f % 64) + ");\n";
                    break;
                default:
                    content += "    return static_cast<int>(hash_val & 0xFF);\n";
                    break;
            }
        }

        // Plant rare needles in a tiny fraction of files
        if (f == num_files / 3) {
            content += "struct SchedEntityRareTarget { uint64_t vruntime_ns; };\n";
            content += "void update_sched_entity_fair_vruntime(SchedEntityRareTarget* se) { se->vruntime_ns++; }\n";
        }
        if (f == (2 * num_files) / 3) {
            content += "int llvm_ir_verify_dominator_tree_unique_42(void* fn) { return fn != nullptr; }\n";
        }
        if (f % 500 == 17) {
            content += "static inline int fast_path_handler_" + std::to_string(f) + "(int x) { return x * 2; }\n";
        }

        content += "}\n";
        std::ofstream out(fp, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
}

void print_latency_row(const char* name, std::vector<double>& samples_ms,
                       const SearchResponse& last) {
    std::sort(samples_ms.begin(), samples_ms.end());
    auto pct = [&](double p) -> double {
        if (samples_ms.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(samples_ms.size() - 1));
        return samples_ms[idx];
    };
    std::printf("  %-34s | p50=%6.3f ms | p95=%6.3f ms | p99=%6.3f ms | cand=%5u | hits=%4llu\n",
                name, pct(0.50), pct(0.95), pct(0.99),
                last.stats.candidate_docs,
                static_cast<unsigned long long>(last.stats.matched_lines));
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path corpus;
    bool temp_corpus = false;
    u32 synth_files = 4000;
    u32 synth_lines = 400;
    int iters = 30;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (a == "--files" && i + 1 < argc) synth_files = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--lines" && i + 1 < argc) synth_lines = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--iters" && i + 1 < argc) iters = std::atoi(argv[++i]);
    }

    if (corpus.empty()) {
        temp_corpus = true;
        corpus = std::filesystem::temp_directory_path() / ("scry_bench_corpus_" + std::to_string(::getpid()));
        std::printf("Generating synthetic corpus (%u files x %u lines) in %s...\n",
                    synth_files, synth_lines, corpus.c_str());
        Stopwatch gsw;
        generate_synthetic_corpus(corpus, synth_files, synth_lines);
        std::printf("Generated in %.1f ms\n\n", gsw.elapsed_ms());
    }

    std::printf("=== scry-bench (SIMD=%s, corpus=%s) ===\n", simd::arch_name(), corpus.c_str());

    // 1. Cold full index build benchmark
    IndexBuildOptions bopts;
    IndexBuildStats bstats;
    std::string err;
    if (!build_index(corpus, bopts, bstats, err)) {
        std::fprintf(stderr, "Index build failed: %s\n", err.c_str());
        return 1;
    }

    double corpus_mb = static_cast<double>(bstats.total_corpus_bytes) / (1024.0 * 1024.0);
    double idx_mb = static_cast<double>(bstats.index_file_bytes) / (1024.0 * 1024.0);
    double ratio = bstats.total_corpus_bytes > 0
        ? static_cast<double>(bstats.index_file_bytes) / static_cast<double>(bstats.total_corpus_bytes)
        : 0.0;

    std::printf("\n[1] Index Build\n");
    std::printf("  Files:      %u (%.2f MiB raw)\n", bstats.files_indexed, corpus_mb);
    std::printf("  Index size: %.2f MiB (%.3fx raw corpus, target <= 0.45x)\n", idx_mb, ratio);
    std::printf("  Build time: %.2f ms (%.1f MiB/s) [io_uring=%s]\n",
                bstats.total_ms, bstats.throughput_mb_s, bstats.used_io_uring ? "yes" : "no");
    std::printf("  Breakdown:  walk=%.1fms read+extract=%.1fms merge+encode=%.1fms write=%.1fms\n",
                bstats.walk_ms, bstats.read_and_extract_ms, bstats.merge_and_encode_ms, bstats.write_ms);

    // Load zero-copy snapshot
    LiveIndex live;
    if (!live.load(corpus, bstats.index_path)) {
        std::fprintf(stderr, "Failed to load mmap index\n");
        return 1;
    }
    auto snap = live.snapshot();

    // 2. Query latency benchmarks (warm mmap)
    std::printf("\n[2] Indexed Query Latency (%d iterations each)\n", iters);

    auto run_case = [&](const char* label, SearchOptions opts) {
        std::vector<double> samples;
        samples.reserve(iters);
        SearchResponse last;
        // Warmup
        last = search_snapshot(*snap, opts);
        for (int i = 0; i < iters; ++i) {
            Stopwatch sw;
            last = search_snapshot(*snap, opts);
            samples.push_back(sw.elapsed_ms());
        }
        print_latency_row(label, samples, last);
    };

    {
        SearchOptions o;
        o.pattern = temp_corpus ? "update_sched_entity_fair_vruntime" : "sched_entity";
        run_case("Rare literal", o);
    }
    {
        SearchOptions o;
        o.pattern = temp_corpus ? "schedentityraretarget" : "sched_entity";
        o.regex_opts.case_insensitive = true;
        run_case("Rare literal (-i icase)", o);
    }
    {
        SearchOptions o;
        o.pattern = temp_corpus ? "(fast_path|update_sched)_.*handler_[0-9]+" : "fn\\s+[a-z0-9_]+_handler";
        run_case("Complex regex", o);
    }
    {
        SearchOptions o;
        o.pattern = "compute_delta_offset";
        o.globs.add("*.hpp");
        run_case("Glob pre-filtered (-g '*.hpp')", o);
    }
    {
        SearchOptions o;
        o.pattern = temp_corpus ? "SchedEntityRareTarget" : "MmapIndex";
        o.def_only = true;
        run_case("Symbol definition (--def)", o);
    }
    {
        SearchOptions o;
        o.pattern = "return";
        o.max_total_matches = 100;
        run_case("Interactive Top-100 ('return')", o);
    }

    // 3. Incremental delta update benchmark (only on synthetic temp corpus so we don't mutate user repos)
    if (temp_corpus) {
        std::printf("\n[3] Incremental Delta Updates\n");
        auto f0 = corpus / "kernel/sched/unit_0.cpp";
        {
            std::ofstream out(f0, std::ios::app);
            out << "\nint incremental_hot_edit_marker = 123;\n";
        }
        auto st1 = live.apply_batch({"kernel/sched/unit_0.cpp"});
        std::printf("  Single-file edit -> searchable: %.3f ms (target < 1.0 ms)\n", st1.elapsed_ms);

        // Simulate 2,000-file git checkout storm
        u32 storm_n = std::min<u32>(2000, synth_files);
        std::vector<std::string> storm_paths;
        storm_paths.reserve(storm_n);
        static const char* kModules[] = {
            "kernel/sched", "kernel/mm", "drivers/net", "drivers/gpu",
            "llvm/IR", "llvm/Transforms", "clang/Sema", "clang/AST",
            "runtime/gc", "engine/index", "engine/query", "ui/tui"
        };
        static const char* kExts[] = {".cpp", ".hpp", ".c", ".h", ".rs", ".py", ".go"};
        for (u32 i = 0; i < storm_n; ++i) {
            storm_paths.push_back(std::string(kModules[i % 12]) + "/unit_" + std::to_string(i) + kExts[i % 7]);
        }
        auto st_storm = live.apply_batch(storm_paths);
        std::printf("  %u-file checkout storm batch:   %.2f ms (target < 200 ms)\n", storm_n, st_storm.elapsed_ms);
    }

    // 4. Memory footprint
    std::printf("\n[4] Resident Memory Footprint\n");
    std::printf("  RSS=%llu KiB (%.2f MiB) | Private_Dirty=%llu KiB (%.2f MiB, target < 25 MiB)\n",
                static_cast<unsigned long long>(current_rss_kib()),
                static_cast<double>(current_rss_kib()) / 1024.0,
                static_cast<unsigned long long>(current_private_dirty_kib()),
                static_cast<double>(current_private_dirty_kib()) / 1024.0);

    if (temp_corpus) {
        std::error_code ec;
        std::filesystem::remove_all(corpus, ec);
        std::filesystem::remove_all(bstats.index_path.parent_path(), ec);
    }
    return 0;
}
