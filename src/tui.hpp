#pragma once

#include "searcher.hpp"

#include <string>

namespace scry {

struct TuiOptions {
    SearchOptions base_opts;
    bool open_in_editor = false; // If true, invoke $EDITOR or zed on Enter instead of printing path:line:col
};

// Launch the zero-lag interactive terminal picker (`scry -I`).
// Returns 0 on selection or clean exit, non-zero on terminal error.
int run_interactive_tui(const TuiOptions& opts);

} // namespace scry
