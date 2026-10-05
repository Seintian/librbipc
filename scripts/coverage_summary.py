#!/usr/bin/env python3
import glob
import os

def main():
    print("=" * 80)
    print(f"{'Source File':<30} | {'Executable Lines':<16} | {'Covered Lines':<14} | {'Coverage %':<10}")
    print("=" * 80)

    total_lines = 0
    total_exec = 0

    gcov_files = sorted(glob.glob("*.gcov"))
    for g in gcov_files:
        basename = os.path.basename(g).replace(".gcov", "")
        # Filter to only librbipc source and internal files
        if not ("rbipc" in basename and not basename.startswith("test_")):
            continue

        with open(g, "r", errors="replace") as f:
            lines = f.readlines()

        code_lines = 0
        exec_lines = 0

        for line in lines:
            parts = line.split(":", 2)
            if len(parts) < 2:
                continue
            count_str = parts[0].strip()
            # Non-executable lines are marked '-'
            if count_str == "-" or count_str.startswith("function") or count_str.startswith("branch"):
                continue

            code_lines += 1
            if count_str != "#####":
                exec_lines += 1

        pct = (exec_lines / code_lines * 100.0) if code_lines > 0 else 100.0
        print(f"{basename:<30} | {code_lines:<16} | {exec_lines:<14} | {pct:>8.2f}%")
        total_lines += code_lines
        total_exec += exec_lines

    print("=" * 80)
    tot_pct = (total_exec / total_lines * 100.0) if total_lines > 0 else 0.0
    print(f"{'TOTAL LINE COVERAGE':<30} | {total_lines:<16} | {total_exec:<14} | {tot_pct:>8.2f}%")
    print("=" * 80)

if __name__ == "__main__":
    main()
