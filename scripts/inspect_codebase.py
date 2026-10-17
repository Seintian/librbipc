import re
import subprocess

def get_public_symbols():
    headers = ["include/rbipc.h", "include/rbipc_ipc.h", "include/rbipc_pubsub.h"]
    declared = set()
    fn_regex = re.compile(r"\b(rbipc_[a-zA-Z0-9_]+)\s*\(")
    for h in headers:
        with open(h, "r") as f:
            content = f.read()
            for m in fn_regex.finditer(content):
                fn = m.group(1)
                if not fn.endswith("_t"):
                    declared.add(fn)
    return declared

def get_exported_so_symbols():
    res = subprocess.run(["nm", "-D", "--defined-only", "lib/librbipc.so"], capture_output=True, text=True)
    exported = set()
    for line in res.stdout.strip().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "D", "B", "R"):
            sym = parts[2].split("@@")[0]
            exported.add(sym)
    return exported

def main():
    pub = get_public_symbols()
    exp = get_exported_so_symbols()
    print(f"Total Public Header API Identifiers: {len(pub)}")
    print(f"Total Dynamic Symbols Exported by librbipc.so: {len(exp)}")
    
    leaked = exp - pub
    print(f"Internal Symbols Leaked: {len(leaked)}")
    for s in sorted(leaked):
        print(f"  - {s}")
        
    in_headers_not_exported = pub - exp
    print(f"Public Header API Identifiers NOT in .so (e.g. static inline): {len(in_headers_not_exported)}")
    for s in sorted(in_headers_not_exported):
        print(f"  - {s}")

if __name__ == "__main__":
    main()
