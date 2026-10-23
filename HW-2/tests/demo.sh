#!/usr/bin/env bash
# Наглядная демонстрация: одна единица модельного времени = 150 мс.
# Во время работы из другого терминала можно послать сигналы:
#   kill -USR1 <pid>  - показать состояние линии
#   kill -USR2 <pid>  - мягкая остановка
# Ctrl+C - остановка, итоги и журнал всё равно сохранятся.
cd "$(dirname "$0")/.." || exit 1
[ -x bin/line_sem ] || make >/dev/null
echo "PID программы: $$"
exec ./bin/line_sem -c "${1:-configs/01_basic.cfg}" -d 150 -l demo.log
