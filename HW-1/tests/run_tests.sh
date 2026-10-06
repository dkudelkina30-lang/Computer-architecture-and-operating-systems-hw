#!/usr/bin/env bash
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/assembly_line
OUT=tests/out
mkdir -p "$OUT"
[ -x "$BIN" ] || make >/dev/null || exit 1
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "  [ OK ] $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $1 — $2"; }

run_case() {
    local name=$1 want=$2 text=$3
    shift 3
    "$BIN" -q -l "$OUT/$name.log" "$@" >"$OUT/$name.out" 2>&1
    local code=$?
    if [ "$code" -ne "$want" ]; then fail "$name" "код $code, ожидался $want"
    elif ! grep -q -- "$text" "$OUT/$name.out"; then fail "$name" "нет строки «$text»"
    elif grep -q "нарушений инвариантов: [1-9]" "$OUT/$name.out"; then fail "$name" "нарушены инварианты"
    else ok "$name (код $code)"
    fi
}

echo "== Сценарии"
run_case 01_basic          0 "ПЛАН ВЫПОЛНЕН"                  -c configs/01_basic.cfg
run_case 02_no_defects     0 "ПЛАН ВЫПОЛНЕН"                  -c configs/02_no_defects.cfg
run_case 03_rework_heavy   0 "ПЛАН ВЫПОЛНЕН"                  -c configs/03_rework_heavy.cfg
run_case 04_raw_exhausted  2 "ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ"      -c configs/04_raw_exhausted.cfg
run_case 05_deadlock       3 "взаимная блокировка"            -c configs/05_deadlock.cfg
run_case 06_no_machine     3 "нет ни одного станка"           -c configs/06_no_machine.cfg
run_case 07_kit_too_big    3 "накопитель вмещает"             -c configs/07_kit_too_big.cfg
run_case 08_bad_syntax     1 "08_bad_syntax.cfg:4"            -c configs/08_bad_syntax.cfg
run_case 09_bottleneck     0 "ПЛАН ВЫПОЛНЕН"                  -c configs/09_bottleneck.cfg
run_case 10_multi_product  0 "WINCH: выпущено 2 из 2"         -c configs/10_multi_product.cfg
run_case builtin           0 "ПЛАН ВЫПОЛНЕН"                  -s 5
run_case timeout           4 "ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ"         -c configs/01_basic.cfg -t 20
run_case missing_file      1 "не удалось открыть"             -c configs/no_such_file.cfg
run_case bad_option        1 "Некорректное значение"          -d abc

echo "== Содержание журналов"
grep -q "\] БРАК" "$OUT/02_no_defects.log" && fail "02" "брак при нулевой вероятности" || ok "02: брака нет"
grep -q "\] ПЕРЕДЕЛКА" "$OUT/03_rework_heavy.log" && ok "03: есть повторная обработка" || fail "03" "нет переделок"
grep -q "станок приостановлен" "$OUT/09_bottleneck.log" && ok "09: станок приостанавливается" || fail "09" "нет приостановки"
missing=""
for ev in СОЗДАНИЕ ОЧЕРЕДЬ ОП.НАЧАЛО ОП.КОНЕЦ КОНТРОЛЬ БРАК ПЕРЕДЕЛКА СПИСАНИЕ ПЕРЕВОЗКА КОМПЛЕКТ СБОРКА ВЫПУСК; do
    grep -q "\] $ev" "$OUT/01_basic.log" || missing="$missing $ev"
done
[ -z "$missing" ] && ok "01: в журнале все обязательные события" || fail "01" "нет событий:$missing"


"$BIN" -q -l "$OUT/rep1.log" -c configs/10_multi_product.cfg -s 99 >/dev/null
"$BIN" -q -l "$OUT/rep2.log" -c configs/10_multi_product.cfg -s 99 >/dev/null
diff <(grep '^\[t=' "$OUT/rep1.log") <(grep '^\[t=' "$OUT/rep2.log") >/dev/null \
    && ok "воспроизводимость по seed" || fail "воспроизводимость" "журналы различаются"

echo "== Завершение по сигналу"
for sig in INT TERM; do
    "$BIN" -d 20 -l "$OUT/signal_$sig.log" -c configs/01_basic.cfg >/dev/null 2>&1 &
    pid=$!                 
    sleep 0.7
    kill -"$sig" "$pid"
    wait "$pid"
    code=$?
    want=$([ "$sig" = INT ] && echo 130 || echo 143)
    if [ "$code" -eq "$want" ] && grep -q "ПРЕРВАНО СИГНАЛОМ" "$OUT/signal_$sig.log"; then
        ok "SIG$sig: итоги сохранены в журнале (код $code)"
    else
        fail "SIG$sig" "код $code, ожидался $want, или журнал не дописан"
    fi
done

echo "Итого: пройдено $PASS, не пройдено $FAIL"
[ "$FAIL" -eq 0 ]
