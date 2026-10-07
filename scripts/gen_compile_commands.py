#!/usr/bin/env python3
import json
import os
import glob

def main():
    cwd = os.getcwd()
    entries = []
    
    # Source objects
    for s in sorted(glob.glob("src/*.c")):
        o = f"build/{os.path.basename(s)[:-2]}.o"
        cmd = f"gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -pthread -D_GNU_SOURCE -c {s} -o {o}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(s)
        })
        
    # Test executables
    for t in sorted(glob.glob("tests/*.c")):
        b = f"bin/{os.path.basename(t)[:-2]}"
        cmd = f"gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -pthread -D_GNU_SOURCE {t} -Llib -lrbipc -pthread -lrt -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t)
        })

    # Benchmark executables
    for b_src in sorted(glob.glob("benchmarks/*.c")):
        b = f"bin/{os.path.basename(b_src)[:-2]}"
        cmd = f"gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -pthread -D_GNU_SOURCE {b_src} -Llib -lrbipc -pthread -lrt -lm -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(b_src)
        })

    with open("compile_commands.json", "w") as f:
        json.dump(entries, f, indent=2)

if __name__ == "__main__":
    main()
