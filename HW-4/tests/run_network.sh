#!/usr/bin/env bash
# Тест двух сетевых узлов Linux через veth (нужны iproute2 и разрешённый unshare).
set -eu
cd "$(dirname "$0")/.."
if [[ ${1:-} != inner ]]; then
    exec unshare -Urn bash "$0" inner
fi

dir=tests/out/network
mkdir -p "$dir"
rm -f "$dir"/*.out "$dir/line.log" "$dir/roles"
port=4404
host=192.0.2.1
cfg=configs/02_no_defects.cfg
pids=()
cleanup()
{
    if ((${#pids[@]})); then kill "${pids[@]}" 2>/dev/null || :; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

ip link set lo up
ip link add hw_server type veth peer name hw_client
ip addr add "$host"/24 dev hw_server
ip link set hw_server up
unshare -n sleep 60 &
node=$!
pids+=("$node")

for ((idx=0; idx<100; idx++)); do
    if [[ $(readlink "/proc/$node/ns/net") != "$(readlink /proc/self/ns/net)" ]]; then break; fi
    sleep .02
done
ip link set hw_client netns "$node"
remote() { exec nsenter -t "$node" -n "$@"; }
(remote ip link set lo up)
(remote ip addr add 192.0.2.2/24 dev hw_client)
(remote ip link set hw_client up)

bin/tcp_server 0.0.0.0 "$port" -c "$cfg" -d 15 -l "$dir/line.log" >"$dir/server.out" 2>&1 &
server=$!
pids+=("$server")

for ((idx=0; idx<100; idx++)); do
    if grep -q 'Сервер слушает' "$dir/server.out" 2>/dev/null; then break; fi
    sleep .02
done
grep -q 'Сервер слушает' "$dir/server.out"
bin/tcp_observer 127.0.0.1 "$port" 0 >"$dir/local.out" 2>&1 &
local_observer=$!
pids+=("$local_observer")
(remote bin/tcp_observer "$host" "$port" 1) >"$dir/remote.out" 2>&1 &
remote_observer=$!
pids+=("$remote_observer")

for ((idx=0; idx<100; idx++)); do
    if grep -q '^Параметры:' "$dir/remote.out" 2>/dev/null && grep -q '^Параметры:' "$dir/local.out" 2>/dev/null; then break; fi
    sleep .02
done
grep -q '^Параметры:' "$dir/remote.out"
grep -q '^Параметры:' "$dir/local.out"
if (remote bin/tcp_control "$host" "$port" list) >"$dir/remote-control.out" 2>&1; then exit 1; fi
grep -q 'Управление доступно с 127.0.0.1' "$dir/remote-control.out"
bin/tcp_server "$host" "$port" -c "$cfg" -L >"$dir/roles"
workers=()

while read -r role idx; do
    (remote bin/tcp_worker "$host" "$port" "$role" "$idx") >"$dir/$role-$idx.out" 2>&1 &
    workers+=("$!")
    pids+=("$!")
done <"$dir/roles"

wait "$server"
wait "$local_observer"
wait "$remote_observer"

for idx in "${workers[@]}"; do wait "$idx"; done
for idx in local remote; do
    grep -q 'ВЫПУСК' "$dir/$idx.out"
    grep -q 'нарушений правил модели: 0' "$dir/$idx.out"
    cmp "$dir/line.log" "$dir/$idx.out"
done

printf 'OK  сервер 192.0.2.1, участники 192.0.2.2, наблюдатели на обоих узлах\n'
