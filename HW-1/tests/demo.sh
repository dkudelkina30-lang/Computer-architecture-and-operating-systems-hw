cd "$(dirname "$0")/.." || exit 1
[ -x bin/assembly_line ] || make >/dev/null
exec ./bin/assembly_line -c "${1:-configs/01_basic.cfg}" -d 120 -l demo.log
