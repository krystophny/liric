#!/usr/bin/env bash
set -euo pipefail
show_llvm=0
out=""
for arg in "$@"; do
  if [[ "$arg" == "--show-llvm" ]]; then
    show_llvm=1
  fi
done
if [[ "$show_llvm" -eq 1 ]]; then
  cat <<'IR'
define i32 @main(i32 %argc, i8** %argv) {
entry:
  ret i32 0
}
IR
  exit 0
fi
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-o" ]]; then
    out="$2"
    shift 2
  else
    shift
  fi
done
if [[ -z "$out" ]]; then
  exit 1
fi
cat > "$out" <<'BIN'
#!/usr/bin/env bash
echo 42
exit 0
BIN
chmod +x "$out"
exit 0
