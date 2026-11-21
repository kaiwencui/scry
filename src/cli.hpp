#pragma once

#include "indexer.hpp"
#include "searcher.hpp"
#include "tui.hpp"

#include <string>

namespace scry {

enum class CliCommand : u8 {
    Search,
    Interactive,
    Index,
    DaemonStart,
    DaemonStop,
    DaemonStatus,
    Watch,
    Unwatch,
    Info,
    TypeList,
    Help,
    Version
};

struct ParsedCli {
    CliCommand cmd = CliCommand::Search;
    SearchOptions search_opts;
    IndexBuildOptions index_opts;
    TuiOptions tui_opts;
    std::filesystem::path cmd_path = ".";
    bool daemon_foreground = false;
    bool has_explicit_cmd_path = false;
    std::string error;
};

ParsedCli parse_cli(int argc, char** argv);
void print_help(std::ostream& out);
void print_version(std::ostream& out);
void print_type_list(std::ostream& out);

} // namespace scry
