#!/usr/bin/env bash
# Запустить линию, всех участников и одного наблюдателя на одном компьютере.
set -u
cd "$(dirname "$0")/.." || exit 1

port="${PORT:-4404}"
tmp=$(mktemp -d)
pids=()
cleanup()
{
    if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || :; fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

bin/tcp_server 127.0.0.1 "$port" -L "$@" >"$tmp/roles" 2>&1 || {
    code=$?
    cat "$tmp/roles"
    exit "$code"
}
bin/tcp_server 127.0.0.1 "$port" -d 100 "$@" >"$tmp/server" 2>&1 &
server=$!
pids+=("$server")
for ((idx=0; idx<100; idx++)); do
    if grep -q 'Сервер слушает' "$tmp/server"; then break; fi
    if ! kill -0 "$server" 2>/dev/null; then
        wait "$server"
        code=$?
        tail -n 10 "$tmp/server"
        exit "$code"
    fi
    sleep .02
done
if ! grep -q 'Сервер слушает' "$tmp/server"; then exit 1; fi

bin/tcp_observer 127.0.0.1 "$port" 0 &
observer=$!
pids+=("$observer")

for ((idx=0; idx<100; idx++)); do
    if bin/tcp_control 127.0.0.1 "$port" list | grep -q 'observer 0: подключён'; then break; fi
    sleep .02
done
while read -r role idx; do
    bin/tcp_worker 127.0.0.1 "$port" "$role" "$idx" >"$tmp/$role-$idx" 2>&1 &
    pids+=("$!")
done <"$tmp/roles"
wait "$server"
code=$?
for idx in "${pids[@]:1}"; do wait "$idx"; done
pids=()
exit "$code"
