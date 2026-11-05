#!/usr/bin/env python3
import json
import os
import glob

def main():
    cwd = os.getcwd()
    cc = os.environ.get("CC", "gcc")
    cstd = os.environ.get("CSTD", "-std=c23")
    entries = []

    # Source objects
    for s in sorted(glob.glob("src/*/*.c")):
        rel = s[4:-2] # strip 'src/' and '.c'
        o = f"build/{rel}.o"
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE -c {s} -o {o}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(s)
        })
        
    # Test executables
    for t in sorted(glob.glob("tests/*/*.c")):
        rel = t[6:-2] # strip 'tests/' and '.c'
        b = f"bin/{rel}"
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE {t} -Llib -lrbipc -pthread -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t)
        })

    # C++ test executables (built with g++ -std=c++20, see Makefile:97)
    cxx = os.environ.get("CXX", "g++")
    for t in sorted(glob.glob("tests/*/*.cpp")):
        rel = t[6:-4] # strip 'tests/' and '.cpp'
        b = f"bin/{rel}"
        cmd = f"{cxx} -std=c++20 -Wall -Wextra -O3 -march=native -Iinclude -Isrc {t} -Llib -lrbipc -pthread -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t)
        })

    # Benchmark executables
    # Multi-source benchmarks (main file + subdirectory sources, see Makefile)
    multi_src = {
        "bench_architecture_suite": "architecture",
        "bench_competitors": "competitors",
        "bench_idle_strategies": "idle",
        "bench_suite": "suite",
        "bench_deep_dive": "deep_dive",
    }
    for b_src in sorted(glob.glob("benchmarks/*.c")):
        base = os.path.basename(b_src)[:-2]
        b = f"bin/{base}"
        inc = ""
        sources = [b_src]
        if base in multi_src:
            subdir = multi_src[base]
            inc = f" -Ibenchmarks/{subdir}"
            sources += sorted(glob.glob(f"benchmarks/{subdir}/*.c"))
        cmd = (f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native "
               f"-fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io "
               f"-Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE{inc} "
               f"{' '.join(sources)} -Llib -lrbipc -pthread -lm -o {b}")
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(b_src)
        })
        # Also emit a per-source entry for each subdirectory benchmark source so
        # clangd/indexers can resolve them individually.
        if base in multi_src:
            subdir = multi_src[base]
            for sub_src in sorted(glob.glob(f"benchmarks/{subdir}/*.c")):
                sub_cmd = (f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 "
                           f"-march=native -fPIC -Iinclude -Isrc -Isrc/core "
                           f"-Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit "
                           f"-Isrc/platform -pthread -D_GNU_SOURCE -Ibenchmarks/{subdir} "
                           f"-c {sub_src} -o /dev/null")
                entries.append({
                    "directory": cwd,
                    "command": sub_cmd,
                    "file": os.path.abspath(sub_src)
                })

    # Tool executables
    for t_src in sorted(glob.glob("tools/*.c")):
        b = f"bin/{os.path.basename(t_src)[:-2]}"
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE {t_src} -Llib -lrbipc -pthread -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t_src)
        })

    with open("compile_commands.json", "w") as f:
        json.dump(entries, f, indent=2)

if __name__ == "__main__":
    main()
