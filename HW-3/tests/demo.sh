#!/usr/bin/env bash
# Наглядная демонстрация: одна единица модельного времени = 150 мс.
# Запуск:  ./tests/demo.sh [unnamed|named|shm|mq] [файл конфигурации]
# Во время работы из другого терминала можно послать сигналы менеджеру:
#   pkill -USR1 -f _manager    (или line_named / line_unnamed) - состояние линии
#   pkill -USR2 -f _manager    - мягкая остановка
# Ctrl+C - остановка; итоги и журнал (demo.log) всё равно сохранятся.
cd "$(dirname "$0")/.." || exit 1
make >/dev/null || exit 1
MODE=${1:-named}
CFG=${2:-configs/01_basic.cfg}
case $MODE in
    unnamed|named) exec ./bin/line_$MODE -c "$CFG" -d 150 -l demo.log ;;
    shm|mq)        exec ./tests/run_independent.sh "$MODE" -c "$CFG" -d 150 -l demo.log ;;
    *)             echo "Использование: $0 [unnamed|named|shm|mq] [файл]" >&2; exit 1 ;;
esac
