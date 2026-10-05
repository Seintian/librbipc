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
        cmd = f"gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -fPIC -Iinclude -pthread -D_GNU_SOURCE {t} -Llib -lrbipc -pthread -lrt -o {b}"
        entries.append({
            "directory": cwd,
            "command": cmd,
            "file": os.path.abspath(t)
        })

    with open("compile_commands.json", "w") as f:
        json.dump(entries, f, indent=2)

if __name__ == "__main__":
    main()
