#!/usr/bin/env bash
# Сценарии линии и проверки клиентов TCP. Вывод каждого прогона в tests/out.
set -u
cd "$(dirname "$0")/.." || exit 1

ip=127.0.0.1
port=${PORT:-$((20000 + $$ % 30000))}
ok=0
bad=0
pids=()
worker_pids=()
mkdir -p tests/out

cleanup()
{
    if ((${#pids[@]})); then
        kill "${pids[@]}" 2>/dev/null || :
        wait "${pids[@]}" 2>/dev/null || :
    fi
    pids=()
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check()
{
    local name=$1
    shift
    if "$@"; then
        printf 'OK  %s\n' "$name"
        ((ok+=1))
    else
        printf 'FAIL %s (см. %s)\n' "$name" "$dir"
        ((bad+=1))
    fi
}

has()
{
    local pattern=$1 file=$2
    for ((idx=0; idx<200; idx++)); do
        if grep -Eq -- "$pattern" "$file" 2>/dev/null; then return 0; fi
        sleep .02
    done
    return 1
}

expect_exit()
{
    local want=$1 pid=$2 code
    wait "$pid" 2>/dev/null
    code=$?
    [[ $code == "$want" ]]
}

start()
{
    local name=$1
    shift
    dir="tests/out/$name"
    mkdir -p "$dir"
    rm -f "$dir"/*.out "$dir/line.log" "$dir/roles"
    worker_pids=()
    bin/tcp_server "$ip" "$port" -d 8 -w 4 -l "$dir/line.log" "$@" >"$dir/server.out" 2>&1 &
    server=$!
    pids+=("$server")
}

watch()
{
    local who=$1 file=$2
    bin/tcp_observer "$ip" "$port" "$who" >"$dir/$file.out" 2>&1 &
    observer=$!
    pids+=("$observer")
}

worker()
{
    local role=$1 who=$2 file=$3
    bin/tcp_worker "$ip" "$port" "$role" "$who" >"$dir/$file.out" 2>&1 &
    participant=$!
    pids+=("$participant")
    worker_pids+=("$participant")
}

workers()
{
    local role idx
    bin/tcp_server "$ip" "$port" -L "$@" >"$dir/roles" || return 1
    while read -r role idx; do worker "$role" "$idx" "$role-$idx"; done <"$dir/roles"
}

pult()
{
    bin/tcp_control "$ip" "$port" "$@"
}

scenario()
{
    local name=$1 want=$2 pattern=$3 idx
    shift 3
    start "$name" "$@"
    if ! has 'Сервер слушает' "$dir/server.out"; then return 1; fi
    watch 0 observer
    has '^Параметры:' "$dir/observer.out" || return 1
    workers "$@" || return 1
    expect_exit "$want" "$server" && expect_exit "$want" "$observer" || return 1
    for idx in "${worker_pids[@]}"; do expect_exit "$want" "$idx" || return 1; done
    grep -q "Завершение: .*$pattern" "$dir/observer.out" &&
        cmp -s "$dir/observer.out" "$dir/line.log" &&
        [[ $(grep -c '^Завершение:' "$dir/observer.out") == 1 ]] &&
        [[ $(tail -n 1 "$dir/observer.out") == '=========================================' ]] &&
        ! grep -q 'Состояние линии' "$dir/observer.out" &&
        grep -q 'нарушений правил модели: 0' "$dir/line.log"
}

invalid()
{
    local name=$1 want=$2 pattern=$3
    shift 3
    start "$name" "$@"
    expect_exit "$want" "$server" && grep -q "$pattern" "$dir/server.out"
}

local_only()
{
    start local_only -c configs/01_basic.cfg
    has 'Сервер слушает' "$dir/server.out" || return 1
    workers -c configs/01_basic.cfg || return 1
    expect_exit 0 "$server" && grep -q 'ОП.НАЧАЛО.*LATHE-1' "$dir/machine-0.out" &&
        ! grep -Eq 'ОП.НАЧАЛО.*MILL|КОНТРОЛЬ|ВЫПУСК' "$dir/machine-0.out" &&
        ! grep -q 'ОП.НАЧАЛО' "$dir/server.out" && grep -q 'ВЫПУСК' "$dir/line.log"
}

dynamic()
{
    local cfg=configs/11_reconnect.cfg first second machine
    start reconnect -c "$cfg" -d 50
    has 'Сервер слушает' "$dir/server.out" || return 1
    watch 0 observer-0; first=$observer
    watch 1 observer-1; second=$observer
    has '^Параметры:' "$dir/observer-0.out" &&
        has '^Параметры:' "$dir/observer-1.out" || return 1
    workers -c "$cfg" || return 1
    machine=${worker_pids[1]}
    has 'ОП.НАЧАЛО.*SHAFT#1' "$dir/observer-0.out" &&
        has 'ОП.НАЧАЛО.*SHAFT#1' "$dir/observer-1.out" || return 1
    kill -TERM "$second" || return 1
    expect_exit 143 "$second" || return 1
    has 'наблюдатель 1 отключился' "$dir/server.out" || return 1
    watch 1 observer-1-rejoined; second=$observer
    has 'LATHE-1\[SHAFT#1\]' "$dir/observer-1-rejoined.out" || return 1
    pult disconnect observer 1 >"$dir/disconnect-observer.out" &&
        expect_exit 0 "$second" || return 1
    watch 1 observer-denied
    expect_exit 1 "$observer" &&
        grep -q 'Подключение запрещено' "$dir/observer-denied.out" &&
        pult allow observer 1 >"$dir/allow-observer.out" || return 1
    watch 1 observer-1-resumed; second=$observer
    has 'LATHE-1\[SHAFT#1\]' "$dir/observer-1-resumed.out" || return 1
    pult disconnect machine 0 >"$dir/disconnect.out" &&
        expect_exit 0 "$machine" || return 1
    worker machine 0 machine-denied
    expect_exit 1 "$participant" &&
        grep -q 'Подключение запрещено' "$dir/machine-denied.out" || return 1
    pult list >"$dir/list.out" &&
        grep -q 'machine 0: запрещён' "$dir/list.out" &&
        pult allow machine 0 >"$dir/allow.out" || return 1
    worker machine 0 machine-rejoined
    has 'участник подключился повторно' "$dir/machine-rejoined.out" &&
        expect_exit 0 "$server" &&
        expect_exit 0 "$first" &&
        expect_exit 0 "$second" &&
        expect_exit 0 "$participant" || return 1
    [[ $(grep -c 'ОП.НАЧАЛО.*SHAFT#1' "$dir/observer-0.out") == 1 ]] &&
        [[ $(grep -c 'ОП.КОНЕЦ.*SHAFT#1' "$dir/observer-0.out") == 1 ]] &&
        grep -q 'ОП.КОНЕЦ.*SHAFT#1' "$dir/machine-rejoined.out" &&
        grep -q 'выпущено 1 из 1' "$dir/observer-1-resumed.out" &&
        grep -q 'нарушений правил модели: 0' "$dir/line.log"
}

stopped()
{
    local signal=$1 want=$2 cfg=configs/11_reconnect.cfg idx
    start "sig$signal" -c "$cfg" -d 50
    has 'Сервер слушает' "$dir/server.out" || return 1
    watch 0 observer
    workers -c "$cfg" || return 1
    has 'ОП.НАЧАЛО.*LATHE-1' "$dir/observer.out" || return 1
    kill "-$signal" "$server" || return 1
    expect_exit "$want" "$server" && expect_exit "$want" "$observer" || return 1
    for idx in "${worker_pids[@]}"; do expect_exit "$want" "$idx" || return 1; done
    grep -q "получен сигнал SIG$signal" "$dir/machine-0.out"
}

missing()
{
    start missing -w 1
    has 'Сервер слушает' "$dir/server.out" || return 1
    worker warehouse 0 warehouse-0
    expect_exit 3 "$server" && expect_exit 3 "$participant" &&
        grep -q 'не все участники подключились' "$dir/warehouse-0.out"
}

lost_worker()
{
    local cfg=configs/11_reconnect.cfg idx
    start lost_worker -c "$cfg" -w 1 -d 50
    has 'Сервер слушает' "$dir/server.out" || return 1
    watch 0 observer
    workers -c "$cfg" || return 1
    has 'ОП.НАЧАЛО.*LATHE-1' "$dir/observer.out" || return 1
    kill -KILL "${worker_pids[1]}" || return 1
    expect_exit 137 "${worker_pids[1]}" &&
        expect_exit 3 "$server" && expect_exit 3 "$observer" || return 1
    for idx in 0 2 3 4; do expect_exit 3 "${worker_pids[idx]}" || return 1; done
    grep -q 'участник не переподключился вовремя' "$dir/observer.out" &&
        grep -q 'нарушений правил модели: 0' "$dir/observer.out"
}

run()
{
    check "$@"
    cleanup
}

plan='ПЛАН ВЫПОЛНЕН'
run '01 обычная линия' scenario 01_basic 0 "$plan" -c configs/01_basic.cfg
run '02 без брака' scenario 02_no_defects 0 "$plan" -c configs/02_no_defects.cfg
run '03 переделки' scenario 03_rework_heavy 0 "$plan" -c configs/03_rework_heavy.cfg
run '04 нехватка заготовок' scenario 04_raw_exhausted 2 'ИСХОДНЫЕ ДЕТАЛИ ИСЧЕРПАНЫ' -c configs/04_raw_exhausted.cfg
run '05 взаимная блокировка' scenario 05_deadlock 3 'взаимная блокировка' -c configs/05_deadlock.cfg
run '06 нет станка' invalid 06_no_machine 3 'нет ни одного станка' -c configs/06_no_machine.cfg
run '07 комплект не помещается' invalid 07_kit_too_big 3 'накопитель вмещает' -c configs/07_kit_too_big.cfg
run '08 ошибка синтаксиса' invalid 08_bad_syntax 1 'Ошибка конфигурации' -c configs/08_bad_syntax.cfg
run '09 узкое место' scenario 09_bottleneck 0 "$plan" -c configs/09_bottleneck.cfg
run '10 несколько изделий' scenario 10_multi_product 0 "$plan" -c configs/10_multi_product.cfg
run 'встроенный сценарий' scenario builtin 0 "$plan" -s 13
run 'лимит времени' scenario timeout 4 'ПРЕВЫШЕН ЛИМИТ ВРЕМЕНИ' -t 10
run 'отсутствующий файл' invalid no_file 1 'не удалось открыть' -c configs/no_file.cfg
run 'неверный ключ' invalid bad_key 1 'Некорректное значение' -d abc
run 'локальный вывод без наблюдателя' local_only
run 'два наблюдателя, повторное подключение наблюдателя и станка' dynamic
run 'SIGINT во время операции' stopped INT 130
run 'SIGTERM во время операции' stopped TERM 143
run 'не все участники подключились' missing
run 'потеря клиента без переподключения' lost_worker

printf 'Итого: пройдено %d, не пройдено %d\n' "$ok" "$bad"
((bad == 0))
