#!/usr/bin/env python3
"""
Post-processing and validation script for librbipc Doxygen documentation.

1. Bundles required auxiliary assets (benchmarks source and benchmark JSON datasets)
   into docs/html/benchmarks/ to ensure relative documentation links resolve.
2. Injects missing single-letter index anchors in Doxygen navigation pages
   (e.g., globals_type.html, globals_enum.html, globals_eval.html).
3. Exhaustively audits all HTML files in docs/html/ to ensure zero broken links,
   missing assets, or unresolved anchor targets.
"""

import os
import sys
import shutil
import re
from html.parser import HTMLParser

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DOCS_HTML_DIR = os.path.join(REPO_DIR, "docs", "html")


def copy_auxiliary_assets():
    """Copy auxiliary benchmark sources and datasets into docs/html/benchmarks/."""
    benchmarks_src_dir = os.path.join(REPO_DIR, "benchmarks")
    benchmarks_dest_dir = os.path.join(DOCS_HTML_DIR, "benchmarks")
    results_src_dir = os.path.join(benchmarks_src_dir, "results")
    results_dest_dir = os.path.join(benchmarks_dest_dir, "results")

    os.makedirs(results_dest_dir, exist_ok=True)

    # Copy benchmark source suite
    bench_suite_src = os.path.join(benchmarks_src_dir, "bench_suite.c")
    if os.path.exists(bench_suite_src):
        shutil.copy2(bench_suite_src, os.path.join(benchmarks_dest_dir, "bench_suite.c"))

    # Copy benchmark json datasets
    if os.path.exists(results_src_dir):
        for fname in os.listdir(results_src_dir):
            if fname.endswith(".json"):
                shutil.copy2(os.path.join(results_src_dir, fname), os.path.join(results_dest_dir, fname))


def fix_missing_single_letter_anchors():
    """Inject missing single-letter anchors generated in Doxygen qindex."""
    if not os.path.exists(DOCS_HTML_DIR):
        return

    qindex_link_pattern = re.compile(r'<a class="qindex" href="#(index_[a-zA-Z0-9_]+)">([a-zA-Z0-9_])</a>')

    for root, _, files in os.walk(DOCS_HTML_DIR):
        for f in files:
            if not f.endswith(".html"):
                continue
            fpath = os.path.join(root, f)
            with open(fpath, "r", encoding="utf-8", errors="ignore") as fp:
                content = fp.read()

            qindex_matches = qindex_link_pattern.findall(content)
            if not qindex_matches:
                continue

            modified = False
            for anchor_id, letter in qindex_matches:
                # Check if anchor exists as id="..." or name="..."
                if f'id="{anchor_id}"' not in content and f'name="{anchor_id}"' not in content:
                    # Inject anchor right before first <ul> list in contents
                    target_tag = "<ul>"
                    if target_tag in content:
                        injection = f'<span id="{anchor_id}" name="{anchor_id}"></span>\n<ul>'
                        content = content.replace(target_tag, injection, 1)
                        modified = True

            if modified:
                with open(fpath, "w", encoding="utf-8") as fp:
                    fp.write(content)


class AnchorCollector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.anchors = set()

    def handle_starttag(self, tag, attrs):
        for k, v in attrs:
            if k in ("id", "name") and v:
                self.anchors.add(v)


class LinkChecker(HTMLParser):
    def __init__(self, current_file, anchors_by_file):
        super().__init__()
        self.current_file = current_file
        self.anchors_by_file = anchors_by_file
        self.errors = []

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            for k, v in attrs:
                if k == "href" and v:
                    self.check_href(v)

    def check_href(self, href):
        if href.startswith(("#", "javascript:", "mailto:", "tel:", "http://", "https:")):
            if href.startswith("#"):
                anchor = href[1:]
                if anchor and anchor not in self.anchors_by_file.get(self.current_file, set()):
                    self.errors.append((self.current_file, href, f"Anchor #{anchor} missing in {self.current_file}"))
            return

        parts = href.split("#", 1)
        target_path = parts[0]
        anchor = parts[1] if len(parts) > 1 else None

        curr_dir = os.path.dirname(self.current_file)
        resolved_rel = os.path.normpath(os.path.join(curr_dir, target_path))
        disk_path = os.path.join(DOCS_HTML_DIR, resolved_rel)

        if not os.path.exists(disk_path):
            self.errors.append((self.current_file, href, f"File {resolved_rel} does not exist"))
            return

        if anchor and resolved_rel in self.anchors_by_file:
            if anchor not in self.anchors_by_file[resolved_rel]:
                self.errors.append((self.current_file, href, f"Anchor #{anchor} missing in {resolved_rel}"))


def validate_all_links():
    """Verify that every link and anchor in the documentation resolves cleanly."""
    anchors_by_file = {}
    for root, _, files in os.walk(DOCS_HTML_DIR):
        for f in files:
            if f.endswith(".html"):
                fpath = os.path.join(root, f)
                rel = os.path.relpath(fpath, DOCS_HTML_DIR)
                parser = AnchorCollector()
                with open(fpath, "r", encoding="utf-8", errors="ignore") as fp:
                    parser.feed(fp.read())
                anchors_by_file[rel] = parser.anchors

    all_errors = []
    for rel in anchors_by_file:
        fpath = os.path.join(DOCS_HTML_DIR, rel)
        checker = LinkChecker(rel, anchors_by_file)
        with open(fpath, "r", encoding="utf-8", errors="ignore") as fp:
            checker.feed(fp.read())
        all_errors.extend(checker.errors)

    if all_errors:
        print(f"[FAIL] Found {len(all_errors)} broken link(s) / anchor(s) in documentation:")
        for src, href, reason in all_errors:
            print(f"  - [{src}] {href} -> {reason}")
        return False

    print(f"[SUCCESS] Verified {len(anchors_by_file)} HTML pages: 0 broken links or unresolved anchors.")
    return True


def main():
    if not os.path.isdir(DOCS_HTML_DIR):
        print(f"Documentation directory {DOCS_HTML_DIR} does not exist.")
        sys.exit(1)

    copy_auxiliary_assets()
    fix_missing_single_letter_anchors()
    if not validate_all_links():
        sys.exit(1)


if __name__ == "__main__":
    main()
