#!/usr/bin/env python3
"""
Reproducible 7-tool code search benchmark matrix for scry.

Lineup:
  One-shot CLI mode:
    1. scry (cold CLI mmapping index.scry)
    2. ripgrep (rg)             - Rust unindexed SIMD baseline & correctness oracle
    3. ugrep (ug)               - C++ unindexed SIMD scanner
    4. ugrep --index (ug -%)    - C++ file-level Bloom/trigram index
    5. git grep                 - C multithreaded tracked-file scanner
    6. csearch                  - Russ Cox Go trigram index
    7. zoekt (CLI)              - Sourcegraph Go trigram shard CLI

  Resident Daemon / Hot-Memory mode:
    1. scryd (AF_UNIX IPC)      - C++23 mmap base + LSM delta
    2. zoekt-webserver (HTTP)   - Sourcegraph Go shard server
    3. livegrep (HTTP -> gRPC)  - Stripe C++ suffix-array server

Fairness rules:
  - Identical repository & git working tree across all tools
  - Reports one-shot CLI mode and resident daemon mode in separate tables
  - Records match counts side-by-side against ripgrep (rg) oracle
"""

import argparse
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path


def find_bin(name, extra_dirs):
    for d in extra_dirs:
        p = Path(d) / name
        if p.exists() and os.access(p, os.X_OK):
            return str(p)
    found = shutil.which(name)
    return found


def proc_mem_mib(pid):
    """Return (rss_mib, private_dirty_mib) from Linux /proc/<pid>/smaps_rollup."""
    rss_kb = 0
    pd_kb = 0
    try:
        with open(f"/proc/{pid}/smaps_rollup", "r") as f:
            for line in f:
                if line.startswith("Rss:"):
                    rss_kb = int(line.split()[1])
                elif line.startswith("Private_Dirty:"):
                    pd_kb = int(line.split()[1])
    except Exception:
        pass
    return rss_kb / 1024.0, pd_kb / 1024.0


def dir_size_bytes(path):
    p = Path(path)
    if p.is_file():
        return p.stat().st_size
    total = 0
    if p.exists():
        for root, _, files in os.walk(p):
            for fn in files:
                try:
                    total += (Path(root) / fn).stat().st_size
                except OSError:
                    pass
    return total


def timed_runs(fn, runs=7, warmup=2):
    """Execute fn() -> match_count, return (p50_ms, p99_ms, last_count)."""
    last_cnt = 0
    for _ in range(warmup):
        last_cnt = fn()
    times = []
    for _ in range(runs):
        t0 = time.perf_counter()
        last_cnt = fn()
        times.append((time.perf_counter() - t0) * 1000.0)
    times.sort()
    p50 = times[len(times) // 2]
    p99 = times[min(len(times) - 1, int(len(times) * 0.99))]
    return p50, p99, last_cnt


def run_cmd_count(cmd, cwd=None, env=None):
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if not p.stdout:
        return 0
    return p.stdout.count(b"\n")


def scryd_rpc_query(sock_path, pattern, target, icase=False, glob_pat=None, max_total=0):
    """Direct AF_UNIX binary RPC client for scryd (no fork/exec overhead)."""
    pat_b = pattern.encode()
    tgt_b = target.encode()
    flags = 1 if icase else 0
    buf = bytearray()
    buf.append(ord("Q"))
    buf += struct.pack("<I", len(pat_b)) + pat_b
    buf += struct.pack("<I", len(tgt_b)) + tgt_b
    buf += struct.pack("<IIIII", flags, 0, 0, 0, max_total)
    if glob_pat:
        gb = glob_pat.encode()
        buf += struct.pack("<II", 1, len(gb)) + gb + struct.pack("<B", 0)
    else:
        buf += struct.pack("<I", 0)
    buf += struct.pack("<II", 0, 0)  # include_types, exclude_types

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.sendall(struct.pack("<I", len(buf)) + buf)

    hdr = b""
    while len(hdr) < 4:
        chunk = s.recv(4 - len(hdr))
        if not chunk:
            s.close()
            return 0
        hdr += chunk
    sz = struct.unpack("<I", hdr)[0]
    data = bytearray()
    while len(data) < sz:
        chunk = s.recv(min(65536, sz - len(data)))
        if not chunk:
            break
        data += chunk
    s.close()
    if not data or data[0] != ord("R"):
        return 0
    # Parse matched_lines directly from response header:
    # 'R' (1) + err_len (4) + err (err_len) + ui (1) + ul (1) + total_docs (4) + cand (4) + ver (4) + mfiles (4) + mlines (8)
    off = 1
    err_len = struct.unpack_from("<I", data, off)[0]
    off += 4 + err_len + 2 + 16
    mlines = struct.unpack_from("<Q", data, off)[0]
    return int(mlines)


def wait_tcp(host, port, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", help="Path to git repository corpus (e.g. /tmp/scry-corpora/linux)")
    ap.add_argument("--scry", default="./scry", help="Path to scry binary")
    ap.add_argument("--tools-dir", default="/tmp/scry-tools", help="Directory with benchmark tools")
    ap.add_argument("--work-dir", default="/var/tmp", help="Scratch directory for multi-GB indexes (needs >15GB free for Livegrep)")
    ap.add_argument("--runs", type=int, default=7, help="Timed runs per query")
    ap.add_argument("--skip-livegrep", action="store_true")
    ap.add_argument("--json-out", default="", help="Optional JSON output file")
    args = ap.parse_args()

    corpus = str(Path(args.corpus).resolve())
    scry_bin = str(Path(args.scry).resolve())
    extra_dirs = [
        f"{args.tools_dir}/bin",
        f"{args.tools_dir}/root/usr/bin",
        "/usr/local/bin",
        "/usr/bin",
    ]

    bins = {
        "scry": scry_bin,
        "rg": find_bin("rg", extra_dirs),
        "git": find_bin("git", extra_dirs),
        "ug": find_bin("ug", extra_dirs),
        "ugrep-indexer": find_bin("ugrep-indexer", extra_dirs),
        "cindex": find_bin("cindex", extra_dirs),
        "csearch": find_bin("csearch", extra_dirs),
        "zoekt-index": find_bin("zoekt-index", extra_dirs),
        "zoekt": find_bin("zoekt", extra_dirs),
        "zoekt-webserver": find_bin("zoekt-webserver", extra_dirs),
        "livegrep-codesearch": find_bin("livegrep-codesearch", extra_dirs),
        "livegrep-web": find_bin("livegrep-web", extra_dirs),
    }

    print("=" * 92)
    print(f"  scry 7-Tool Code Search Benchmark Matrix")
    print("=" * 92)
    print(f"Corpus: {corpus}")
    try:
        sha = subprocess.check_output(["git", "-C", corpus, "rev-parse", "--short", "HEAD"], text=True).strip()
        nfiles = len(subprocess.check_output(["git", "-C", corpus, "ls-files"], text=True).splitlines())
        print(f"Git HEAD: {sha} | Tracked files: {nfiles}")
    except Exception:
        pass
    print("Detected tools:", ", ".join(k for k, v in bins.items() if v))
    print()

    os.makedirs(args.work_dir, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="scry_matrix_", dir=args.work_dir))
    procs = []

    try:
        # =====================================================================
        # PHASE 1: INDEX BUILD TIME & ON-DISK SIZE
        # =====================================================================
        build_stats = {}

        # 1a. scry index
        scry_cache = work / "scry_cache"
        scry_cache.mkdir()
        scry_env = os.environ.copy()
        scry_env["SCRY_CACHE_DIR"] = str(scry_cache)
        scry_sock = str(work / "scryd.sock")
        scry_env["SCRY_SOCKET"] = scry_sock

        t0 = time.perf_counter()
        p = subprocess.run([bins["scry"], "index", corpus], env=scry_env, capture_output=True, text=True, check=True)
        scry_build_s = time.perf_counter() - t0
        scry_idx_bytes = dir_size_bytes(scry_cache)
        # Parse raw corpus MiB from scry index output
        corpus_mib = 0.0
        for tok in p.stdout.split():
            if tok.startswith("(") and tok[1:].replace(".", "", 1).isdigit():
                corpus_mib = float(tok[1:])
                break
        build_stats["scry"] = {
            "build_s": scry_build_s,
            "size_mib": scry_idx_bytes / (1024 * 1024),
            "throughput_mibs": corpus_mib / scry_build_s if scry_build_s > 0 else 0,
        }
        print(f"[build] scry:          {scry_build_s:6.2f} s | {scry_idx_bytes/(1024*1024):7.1f} MiB ({(scry_idx_bytes/(1024*1024))/max(corpus_mib,1):.2f}x) | {build_stats['scry']['throughput_mibs']:6.1f} MiB/s")

        # 1b. ugrep-indexer (-I ignore binary, -X respect .gitignore, skip hidden/.git)
        ug_indexed = False
        if bins["ugrep-indexer"] and bins["ug"]:
            subprocess.run([bins["ugrep-indexer"], "-d", corpus], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            t0 = time.perf_counter()
            subprocess.run([bins["ugrep-indexer"], "-I", "-X", corpus], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            ug_build_s = time.perf_counter() - t0
            ug_bytes = 0
            for root, _, files in os.walk(corpus):
                for fn in files:
                    if fn.startswith("._UG#_"):
                        try:
                            ug_bytes += (Path(root) / fn).stat().st_size
                        except OSError:
                            pass
            ug_indexed = True
            build_stats["ug --index"] = {
                "build_s": ug_build_s,
                "size_mib": ug_bytes / (1024 * 1024),
                "throughput_mibs": corpus_mib / ug_build_s if ug_build_s > 0 else 0,
            }
            print(f"[build] ugrep-indexer: {ug_build_s:6.2f} s | {ug_bytes/(1024*1024):7.1f} MiB ({(ug_bytes/(1024*1024))/max(corpus_mib,1):.2f}x) | {build_stats['ug --index']['throughput_mibs']:6.1f} MiB/s", flush=True)

        # 1c. cindex (Russ Cox codesearch)
        csearch_env = os.environ.copy()
        csearch_idx = work / "csearch.idx"
        csearch_env["CSEARCHINDEX"] = str(csearch_idx)
        if bins["cindex"] and bins["csearch"]:
            t0 = time.perf_counter()
            subprocess.run([bins["cindex"], corpus], env=csearch_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            cs_build_s = time.perf_counter() - t0
            cs_bytes = dir_size_bytes(csearch_idx)
            build_stats["csearch"] = {
                "build_s": cs_build_s,
                "size_mib": cs_bytes / (1024 * 1024),
                "throughput_mibs": corpus_mib / cs_build_s if cs_build_s > 0 else 0,
            }
            print(f"[build] cindex:        {cs_build_s:6.2f} s | {cs_bytes/(1024*1024):7.1f} MiB ({(cs_bytes/(1024*1024))/max(corpus_mib,1):.2f}x) | {build_stats['csearch']['throughput_mibs']:6.1f} MiB/s")

        # 1d. Zoekt (zoekt-index)
        zoekt_dir = work / "zoekt_idx"
        zoekt_dir.mkdir()
        if bins["zoekt-index"]:
            t0 = time.perf_counter()
            subprocess.run(
                [bins["zoekt-index"], "-index", str(zoekt_dir), "-parallelism", "16", corpus],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True
            )
            zk_build_s = time.perf_counter() - t0
            zk_bytes = dir_size_bytes(zoekt_dir)
            build_stats["Zoekt"] = {
                "build_s": zk_build_s,
                "size_mib": zk_bytes / (1024 * 1024),
                "throughput_mibs": corpus_mib / zk_build_s if zk_build_s > 0 else 0,
            }
            print(f"[build] zoekt-index:   {zk_build_s:6.2f} s | {zk_bytes/(1024*1024):7.1f} MiB ({(zk_bytes/(1024*1024))/max(corpus_mib,1):.2f}x) | {build_stats['Zoekt']['throughput_mibs']:6.1f} MiB/s")

        # 1e. Livegrep (codesearch -index_only -dump_index)
        lg_idx = work / "livegrep.idx"
        lg_cfg = work / "livegrep.json"
        if not args.skip_livegrep and bins["livegrep-codesearch"] and bins["livegrep-web"]:
            cfg = {
                "name": "bench",
                "repositories": [{
                    "path": corpus,
                    "name": "linux",
                    "revisions": ["HEAD"],
                }]
            }
            lg_cfg.write_text(json.dumps(cfg))
            t0 = time.perf_counter()
            lg_res = subprocess.run(
                [bins["livegrep-codesearch"], "-index_only", "-dump_index", str(lg_idx),
                 "-threads", "16", str(lg_cfg)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
            )
            if lg_res.returncode == 0 and lg_idx.exists():
                lg_build_s = time.perf_counter() - t0
                lg_bytes = dir_size_bytes(lg_idx)
                build_stats["Livegrep"] = {
                    "build_s": lg_build_s,
                    "size_mib": lg_bytes / (1024 * 1024),
                    "throughput_mibs": corpus_mib / lg_build_s if lg_build_s > 0 else 0,
                }
                print(f"[build] livegrep:      {lg_build_s:6.2f} s | {lg_bytes/(1024*1024):7.1f} MiB ({(lg_bytes/(1024*1024))/max(corpus_mib,1):.2f}x) | {build_stats['Livegrep']['throughput_mibs']:6.1f} MiB/s", flush=True)
            else:
                print(f"[build] livegrep:      FAILED (rc={lg_res.returncode}): {lg_res.stderr.strip()[:200]}", flush=True)

        # =====================================================================
        # PHASE 2: START RESIDENT DAEMONS & RECORD RSS AT REST
        # =====================================================================
        print("\n--- Starting Resident Daemons ---")
        daemon_rss = {}

        # 2a. scryd (-f keeps daemon in foreground so benchmark owns child PID directly)
        scryd_proc = subprocess.Popen(
            [bins["scry"], "daemon", "start", "-f", corpus],
            env=scry_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        procs.append(scryd_proc)
        for _ in range(100):
            if os.path.exists(scry_sock):
                break
            time.sleep(0.02)
        time.sleep(0.2)
        daemon_rss["scryd"] = proc_mem_mib(scryd_proc.pid)
        print(f"[daemon] scryd:           RSS={daemon_rss['scryd'][0]:7.1f} MiB | Private_Dirty={daemon_rss['scryd'][1]:7.2f} MiB")

        # 2b. zoekt-webserver (-rpc enables /api/search JSON endpoint)
        zk_port = 16070
        zk_ready = False
        if bins["zoekt-webserver"] and "Zoekt" in build_stats:
            zk_proc = subprocess.Popen(
                [bins["zoekt-webserver"], "-rpc", "-index", str(zoekt_dir), "-listen", f"127.0.0.1:{zk_port}"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            procs.append(zk_proc)
            zk_ready = wait_tcp("127.0.0.1", zk_port, timeout=20.0)
            time.sleep(0.3)
            daemon_rss["Zoekt"] = proc_mem_mib(zk_proc.pid)
            print(f"[daemon] zoekt-webserver: RSS={daemon_rss['Zoekt'][0]:7.1f} MiB | Private_Dirty={daemon_rss['Zoekt'][1]:7.2f} MiB")

        # 2c. livegrep (codesearch gRPC + livegrep-web HTTP)
        lg_grpc_port = 19999
        lg_http_port = 18910
        lg_ready = False
        if "Livegrep" in build_stats:
            lg_cs_proc = subprocess.Popen(
                [bins["livegrep-codesearch"], "-load_index", str(lg_idx),
                 "-grpc", f"127.0.0.1:{lg_grpc_port}", "-threads", "16", "-max_matches", "5000"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            procs.append(lg_cs_proc)
            if wait_tcp("127.0.0.1", lg_grpc_port, timeout=30.0):
                lg_web_cmd = [
                    bins["livegrep-web"],
                    "-connect", f"127.0.0.1:{lg_grpc_port}",
                    "-listen", f"127.0.0.1:{lg_http_port}",
                ]
                lg_docroot = Path(args.tools_dir) / "livegrep-web-docroot"
                if lg_docroot.exists():
                    lg_web_cmd += ["-docroot", str(lg_docroot)]
                lg_web_proc = subprocess.Popen(
                    lg_web_cmd,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )
                procs.append(lg_web_proc)
                lg_ready = wait_tcp("127.0.0.1", lg_http_port, timeout=15.0)
                time.sleep(0.3)
                r1, pd1 = proc_mem_mib(lg_cs_proc.pid)
                r2, pd2 = proc_mem_mib(lg_web_proc.pid)
                daemon_rss["Livegrep"] = (r1 + r2, pd1 + pd2)
                print(f"[daemon] livegrep:        RSS={r1+r2:7.1f} MiB (codesearch={r1:.1f}) | Private_Dirty={pd1+pd2:7.2f} MiB")

        # =====================================================================
        # PHASE 3: QUERY BENCHMARKS (BOTH MODES + MATCH-COUNT VERIFICATION)
        # =====================================================================
        queries = [
            {
                "name": "Q1: Rare literal ('update_curr_dl_se')",
                "pattern": "update_curr_dl_se",
                "icase": False,
                "is_regex": False,
                "glob": None,
                "zoekt_q": "case:yes update_curr_dl_se",
                "lg_q": "update_curr_dl_se",
            },
            {
                "name": "Q2: Medium literal ('sched_entity')",
                "pattern": "sched_entity",
                "icase": False,
                "is_regex": False,
                "glob": None,
                "zoekt_q": "case:yes sched_entity",
                "lg_q": "sched_entity",
            },
            {
                "name": "Q3: Case-insensitive (-i 'SCHED_FLAG_UTIL_CLAMP')",
                "pattern": "SCHED_FLAG_UTIL_CLAMP",
                "icase": True,
                "is_regex": False,
                "glob": None,
                "zoekt_q": "case:no SCHED_FLAG_UTIL_CLAMP",
                "lg_q": "SCHED_FLAG_UTIL_CLAMP",
            },
            {
                "name": "Q4: Complex regex ('struct\\s+sched_[a-z_]+_entity')",
                "pattern": r"struct\s+sched_[a-z_]+_entity",
                "icase": False,
                "is_regex": True,
                "glob": None,
                "zoekt_q": r"case:yes regex:struct\s+sched_[a-z_]+_entity",
                "lg_q": r"struct\s+sched_[a-z_]+_entity",
            },
            {
                "name": "Q5: Header-filtered ('rq_clock_pelt' in *.h)",
                "pattern": "rq_clock_pelt",
                "icase": False,
                "is_regex": False,
                "glob": "*.h",
                "zoekt_q": r"case:yes f:\.h$ rq_clock_pelt",
                "lg_q": r"rq_clock_pelt file:\.h$",
            },
            {
                "name": "Q6: Common literal ('vruntime')",
                "pattern": "vruntime",
                "icase": False,
                "is_regex": False,
                "glob": None,
                "zoekt_q": "case:yes vruntime",
                "lg_q": "vruntime",
            },
        ]

        results = {"build": build_stats, "daemon_rss": daemon_rss, "queries": []}

        for q in queries:
            print("\n" + "=" * 92)
            print(f"  {q['name']}")
            print("=" * 92)
            pat = q["pattern"]
            ic = q["icase"]
            gl = q["glob"]

            q_rec = {"name": q["name"], "cli": {}, "daemon": {}}

            # --- MODE A: ONE-SHOT CLI ---
            print("  [One-Shot CLI Mode]")
            cli_engines = []

            # 1. scry (cold CLI mmapping index.scry, --no-daemon)
            scry_cmd = [bins["scry"], "--no-daemon", "--color=never"]
            if ic: scry_cmd.append("-i")
            if gl: scry_cmd += ["-g", gl]
            scry_cmd += [pat, corpus]
            cli_engines.append(("scry (cold CLI mmap)", lambda c=scry_cmd: run_cmd_count(c, env=scry_env)))

            # 2. ripgrep (oracle)
            if bins["rg"]:
                rg_cmd = [bins["rg"], "--no-heading", "--color=never", "-n", "-H"]
                if ic: rg_cmd.append("-i")
                if gl: rg_cmd += ["-g", gl]
                rg_cmd += [pat, corpus]
                cli_engines.append(("ripgrep (rg)", lambda c=rg_cmd: run_cmd_count(c)))

            # 3. git grep
            if bins["git"]:
                gg_cmd = [bins["git"], "-C", corpus, "grep", "-n", "-E"]
                if ic: gg_cmd.append("-i")
                # Note: git grep -E uses POSIX ERE (\s -> [[:space:]])
                gpat = pat.replace(r"\s", "[[:space:]]")
                gg_cmd += [gpat]
                if gl: gg_cmd += ["--", f":(glob)**/{gl}"]
                cli_engines.append(("git grep", lambda c=gg_cmd: run_cmd_count(c)))

            # 4. ugrep unindexed
            if bins["ug"]:
                ug_cmd = [bins["ug"], "-r", "-n", "-I", "--ignore-files", "--color=never"]
                if ic: ug_cmd.append("-i")
                if gl: ug_cmd += ["--include", gl]
                ug_cmd += [pat, corpus]
                cli_engines.append(("ugrep (unindexed)", lambda c=ug_cmd: run_cmd_count(c)))

            # 5. ugrep --index
            if ug_indexed and bins["ug"]:
                ugi_cmd = [bins["ug"], "--index", "-r", "-n", "-I", "--ignore-files", "--color=never"]
                if ic: ugi_cmd.append("-i")
                if gl: ugi_cmd += ["--include", gl]
                ugi_cmd += [pat, corpus]
                cli_engines.append(("ugrep --index", lambda c=ugi_cmd: run_cmd_count(c)))

            # 6. csearch (Russ Cox)
            if bins["csearch"] and "csearch" in build_stats:
                cs_cmd = [bins["csearch"], "-n"]
                if ic: cs_cmd.append("-i")
                if gl: cs_cmd += ["-f", r"\.h$"]
                cs_cmd.append(pat)
                cli_engines.append(("csearch (Russ Cox)", lambda c=cs_cmd: run_cmd_count(c, env=csearch_env)))

            # 7. zoekt CLI
            if bins["zoekt"] and "Zoekt" in build_stats:
                zk_cmd = [bins["zoekt"], "-index_dir", str(zoekt_dir), q["zoekt_q"]]
                cli_engines.append(("zoekt (CLI)", lambda c=zk_cmd: run_cmd_count(c)))

            for label, fn in cli_engines:
                p50, p99, cnt = timed_runs(fn, runs=args.runs, warmup=1)
                q_rec["cli"][label] = {"p50_ms": p50, "p99_ms": p99, "matches": cnt}
                print(f"    {label:<26} p50={p50:8.2f} ms | p99={p99:8.2f} ms | matches={cnt}")

            # --- MODE B: RESIDENT DAEMON / HOT-MEMORY ---
            print("  [Resident Daemon / Hot-Memory Mode]")
            daemon_engines = []

            # 1a. scryd direct AF_UNIX RPC (apples-to-apples daemon RPC vs Zoekt/Livegrep HTTP!)
            daemon_engines.append((
                "scryd (AF_UNIX IPC)",
                lambda p=pat, ic_=ic, g=gl: scryd_rpc_query(scry_sock, p, corpus, ic_, g)
            ))

            # 1b. scryd via CLI AF_UNIX client (end-to-end incl. process spawn)
            scryd_cmd = [bins["scry"], "--color=never"]
            if ic: scryd_cmd.append("-i")
            if gl: scryd_cmd += ["-g", gl]
            scryd_cmd += [pat, corpus]
            daemon_engines.append(("scryd (CLI -> AF_UNIX)", lambda c=scryd_cmd: run_cmd_count(c, env=scry_env)))

            # 2. Zoekt webserver HTTP JSON API
            if zk_ready:
                def query_zoekt(zq=q["zoekt_q"]):
                    body = json.dumps({"Q": zq, "Opts": {"MaxDocDisplayCount": 10000, "NumContextLines": 0}}).encode()
                    req = urllib.request.Request(
                        f"http://127.0.0.1:{zk_port}/api/search",
                        data=body,
                        headers={"Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(req, timeout=15) as resp:
                        data = json.loads(resp.read())
                    total = 0
                    for f in data.get("Result", {}).get("Files") or []:
                        total += len(f.get("LineMatches") or [])
                    return total
                daemon_engines.append(("Zoekt (HTTP daemon)", query_zoekt))

            # 3. Livegrep HTTP API -> C++ suffix-array gRPC server
            if lg_ready:
                def query_livegrep(lq=q["lg_q"], ic_=ic):
                    fc = "true" if ic_ else "false"
                    url = f"http://127.0.0.1:{lg_http_port}/api/v1/search/?q=" + urllib.parse.quote(lq) + f"&fold_case={fc}&max_matches=5000"
                    with urllib.request.urlopen(url, timeout=15) as resp:
                        data = json.loads(resp.read())
                    return len(data.get("results") or [])
                daemon_engines.append(("Livegrep (HTTP->gRPC)", query_livegrep))

            for label, fn in daemon_engines:
                try:
                    p50, p99, cnt = timed_runs(fn, runs=args.runs, warmup=2)
                    q_rec["daemon"][label] = {"p50_ms": p50, "p99_ms": p99, "matches": cnt}
                    print(f"    {label:<26} p50={p50:8.2f} ms | p99={p99:8.2f} ms | matches={cnt}")
                except Exception as exc:
                    print(f"    {label:<26} ERROR: {exc}")

            results["queries"].append(q_rec)

        # Record post-query warm RSS for all daemons
        print("\n--- Post-Query Resident Daemon Memory (Warm) ---")
        warm_rss = {}
        warm_rss["scryd"] = proc_mem_mib(scryd_proc.pid)
        print(f"[daemon-warm] scryd:           RSS={warm_rss['scryd'][0]:7.1f} MiB | Private_Dirty={warm_rss['scryd'][1]:7.2f} MiB")
        if zk_ready:
            warm_rss["Zoekt"] = proc_mem_mib(zk_proc.pid)
            print(f"[daemon-warm] zoekt-webserver: RSS={warm_rss['Zoekt'][0]:7.1f} MiB | Private_Dirty={warm_rss['Zoekt'][1]:7.2f} MiB")
        if lg_ready:
            r1, pd1 = proc_mem_mib(lg_cs_proc.pid)
            r2, pd2 = proc_mem_mib(lg_web_proc.pid)
            warm_rss["Livegrep"] = (r1 + r2, pd1 + pd2)
            print(f"[daemon-warm] livegrep:        RSS={r1+r2:7.1f} MiB (codesearch={r1:.1f}) | Private_Dirty={pd1+pd2:7.2f} MiB")
        results["daemon_rss_warm"] = warm_rss

        if args.json_out:
            Path(args.json_out).write_text(json.dumps(results, indent=2))
            print(f"\nSaved JSON report to {args.json_out}")

    finally:
        # Stop daemons cleanly
        subprocess.run([bins["scry"], "daemon", "stop"], env={"SCRY_SOCKET": str(work / "scryd.sock"), **os.environ},
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for p in procs:
            try:
                p.terminate()
                p.wait(timeout=3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        # Clean ugrep index sidecars from corpus so repo remains spotless
        if bins["ugrep-indexer"]:
            subprocess.run([bins["ugrep-indexer"], "-d", corpus], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
