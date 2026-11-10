#!/usr/bin/env bash
#-------------------------------------------------------------------------
#  run_tests.sh - автоматический прогон тестов всех четырёх программ
#  Запуск:  ./tests/run_tests.sh            (или make test)
#           ./tests/run_tests.sh mq         (только одна программа)
#-------------------------------------------------------------------------
#  Программы: unnamed (line_unnamed), named (line_named),
#             shm (shm_manager + shm_worker), mq (mq_manager + mq_worker).
#  Для каждой проверяются код завершения, итоговое сообщение, отсутствие
#  нарушений правил модели, удаление объектов IPC после завершения
#  и реакция на сигналы. Вывод и журналы - в tests/out/.
#-------------------------------------------------------------------------

cd "$(dirname "$0")/.." || exit 1
make >/dev/null || exit 1
PROGS=${*:-unnamed named shm mq}
PASS=0
FAIL=0
set -m     # фоновые задания - в своих группах процессов (для Ctrl+C группе)

ok()   { PASS=$((PASS + 1)); echo "  [ OK ] $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $1 - $2"; }

# Запустить программу P с именем объектов IPC N и ключами менеджера
run() {
    local p=$1 n=$2
    shift 2
    case $p in
        unnamed) ./bin/line_unnamed -n "$n" "$@" ;;
        named)   ./bin/line_named -n "$n" "$@" ;;
        *)       IPC_NAME=$n ./tests/run_independent.sh "$p" "$@" ;;
    esac
}

# Остались ли объекты IPC с именем n (память, семафоры, очереди)
leftovers() {
    ls /dev/shm 2>/dev/null | grep -q "$1_" && return 0
    ls /dev/mqueue 2>/dev/null | grep -q "$1_" && return 0
    return 1
}

# case_ ПРОГРАММА ИМЯ КОД СТРОКА [ключи...]
case_() {
    local p=$1 name=$2 want=$3 text=$4
    shift 4
    local n="t_${p}_$name" out="$OUT/$name"
    timeout 120 bash -c "$(declare -f run); run $p $n -q -d 5 -l $out.log $(printf '%q ' "$@")" >"$out.out" 2>&1
    local code=$?
    if [ "$code" -ne "$want" ]; then fail "$name" "код $code, ожидался $want"
    elif ! grep -q -- "$text" "$out.out"; then fail "$name" "нет строки «$text»"
    elif grep -q "нарушений правил модели: [1-9]" "$out.out"; then fail "$name" "нарушены правила модели"
    elif leftovers "$n"; then fail "$name" "не удалены объекты IPC"
    else ok "$name (код $code)"
    fi
}

# Послать сигнал менеджеру (или группе) во время работы; sig_case ПРОГРАММА ИМЯ СИГНАЛ КОД СТРОКА
sig_case() {
    local p=$1 name=$2 sig=$3 want=$4 text=$5 n="s_${p}_$2"
    run "$p" "$n" -d 30 -l "$OUT/$name.log" -c configs/01_basic.cfg >"$OUT/$name.out" 2>&1 &
    local job=$!
    sleep 0.8
    case $sig in
        GROUP_INT) kill -INT -- -"$job" ;;                                    # как Ctrl+C в терминале
        USR)       pkill -USR1 -f "_(manager|unnamed|named) -n $n( |$)" || kill -USR1 "$job"
                   sleep 0.3
                   pkill -USR2 -f "_(manager|unnamed|named) -n $n( |$)" || kill -USR2 "$job" ;;
        KILL_WORKER) pkill -TERM -f "_worker -n $n machine 3" ;;              # досрочно уходит DRILL-1
        *)         pkill -"$sig" -f "_(manager|unnamed|named) -n $n( |$)" ;;
    esac
    wait "$job"
    local code=$?
    if [ "$code" -ne "$want" ]; then fail "$name" "код $code, ожидался $want"
    elif ! grep -q -- "$text" "$OUT/$name.out" "$OUT/$name.log"; then fail "$name" "нет строки «$text»"
    elif leftovers "$n"; then fail "$name" "не удалены объекты IPC"
    else ok "$name (код $code)"
    fi
}

for P in $PROGS; do
    OUT=tests/out/$P
    rm -rf "$OUT"
    mkdir -p "$OUT"
    echo "=== Программа: $P"
    case_ "$P" 01_basic          0 "ПЛАН ВЫПОЛНЕН"              -c configs/01_basic.cfg
    case_ "$P" 02_no_defects     0 "ПЛАН ВЫПОЛНЕН"              -c configs/02_no_defects.cfg
    case_ "$P" 03_rework_heavy   0 "ПЛАН ВЫПОЛНЕН"              -c configs/03_rework_heavy.cfg
    case_ "$P" 04_raw_exhausted  2 "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ"  -c configs/04_raw_exhausted.cfg
    case_ "$P" 05_deadlock       3 "взаимная блокировка"        -c configs/05_deadlock.cfg
    case_ "$P" 06_no_machine     3 "нет ни одного станка"       -c configs/06_no_machine.cfg
    case_ "$P" 07_kit_too_big    3 "накопитель вмещает"         -c configs/07_kit_too_big.cfg
    case_ "$P" 08_bad_syntax     1 "08_bad_syntax.cfg:[0-9]*: неизвестный тип станка" -c configs/08_bad_syntax.cfg
    case_ "$P" 09_bottleneck     0 "ПЛАН ВЫПОЛНЕН"              -c configs/09_bottleneck.cfg
    case_ "$P" 10_multi_product  0 "WINCH: выпущено 2 из 2"     -c configs/10_multi_product.cfg
    case_ "$P" builtin           0 "ПЛАН ВЫПОЛНЕН"
    case_ "$P" timeout           4 "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ"     -c configs/01_basic.cfg -t 20
    case_ "$P" missing_file      1 "не удалось открыть"         -c configs/no_such_file.cfg
    case_ "$P" bad_option        1 "Некорректное значение"      -s abc

    missing=""
    for ev in СОЗДАНИЕ ОЧЕРЕДЬ ОП.НАЧАЛО ОП.КОНЕЦ КОНТРОЛЬ ПЕРЕВОЗКА КОМПЛЕКТ СБОРКА ВЫПУСК; do
        grep -q "\] $ev" "$OUT/01_basic.log" || missing="$missing $ev"
    done
    [ -z "$missing" ] && ok "журнал: все основные события" || fail "журнал" "нет событий:$missing"
    n_conn=$(grep -c "процесс подключился" "$OUT/01_basic.log")
    n_exit=$(grep -c "процесс завершился" "$OUT/01_basic.log")
    [ "$n_conn" -eq 10 ] && [ "$n_exit" -eq 10 ] && ok "журнал: 10 процессов подключились и завершились" \
        || fail "процессы" "подключилось $n_conn, завершилось $n_exit"
    grep -q "\] ПЕРЕДЕЛКА" "$OUT/03_rework_heavy.log" && ok "03: есть повторная обработка" || fail "03" "нет переделок"

    sig_case "$P" sigint  GROUP_INT 130 "ПРЕРВАНО СИГНАЛОМ"
    sig_case "$P" sigterm TERM      143 "получен сигнал SIGTERM"
    sig_case "$P" sigusr  USR       140 "МЯГКАЯ ОСТАНОВКА"
    grep -q "Состояние линии" "$OUT/sigusr.out" && ok "SIGUSR1: выведено состояние линии" || fail "SIGUSR1" "нет состояния"
    if [ "$P" = shm ] || [ "$P" = mq ]; then
        sig_case "$P" worker_quit KILL_WORKER 3 "процессов-участников осталось 9 из 10"
    fi
done

echo "Итого: пройдено $PASS, не пройдено $FAIL"
[ "$FAIL" -eq 0 ]
