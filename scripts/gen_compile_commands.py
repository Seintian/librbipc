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
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE {t} -Llib -lrbipc -pthread -lrt -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t)
        })

    # Benchmark executables
    for b_src in sorted(glob.glob("benchmarks/*.c")):
        b = f"bin/{os.path.basename(b_src)[:-2]}"
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE {b_src} -Llib -lrbipc -pthread -lrt -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(b_src)
        })

    # Tool executables
    for t_src in sorted(glob.glob("tools/*.c")):
        b = f"bin/{os.path.basename(t_src)[:-2]}"
        cmd = f"{cc} {cstd} -Wall -Wextra -Wpedantic -Werror -O3 -march=native -fPIC -Iinclude -Isrc -Isrc/core -Isrc/memory -Isrc/sync -Isrc/io -Isrc/ipc -Isrc/audit -Isrc/platform -pthread -D_GNU_SOURCE {t_src} -Llib -lrbipc -pthread -lrt -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t_src)
        })

    with open("compile_commands.json", "w") as f:
        json.dump(entries, f, indent=2)

if __name__ == "__main__":
    main()
