#!/usr/bin/env python3
"""
Doxygen input filter for Markdown files.

Translates LaTeX math syntax ($...$ and $$...$$) to Doxygen formulas
(@f$...@f$ and @f[...@f]) outside fenced code blocks, ensuring clean
MathJax rendering without Doxygen command parsing warnings.
"""

import sys
import re


def filter_markdown(content: str) -> str:
    # Protect fenced code blocks from formula substitution
    parts = content.split("```")
    for i in range(0, len(parts), 2):
        chunk = parts[i]
        # Replace LaTeX section symbol macro with UTF-8 character
        chunk = chunk.replace(r"$\S$", "§")
        chunk = re.sub(r"\$\\S\$([0-9])", r"§\1", chunk)
        # Display math: $$ ... $$ -> @f[ ... @f]
        chunk = re.sub(r"\$\$(.*?)\$\$", r"@f[\1@f]", chunk, flags=re.DOTALL)
        # Inline math: $ ... $ -> @f$ ... @f$
        chunk = re.sub(r"\$([^$\n]+)\$", r"@f$\1@f$", chunk)
        parts[i] = chunk
    return "```".join(parts)


def main():
    if len(sys.argv) < 2:
        return
    file_path = sys.argv[1]
    with open(file_path, "r", encoding="utf-8", errors="ignore") as fp:
        content = fp.read()

    filtered = filter_markdown(content)
    sys.stdout.write(filtered)


if __name__ == "__main__":
    main()
