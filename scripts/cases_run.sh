#!/bin/bash

set -u

QNX_HOST="qnx"
DURATION_S=30

while getopts "h:d:" opt; do
    case "$opt" in
    h) QNX_HOST="$OPTARG" ;;
    d) DURATION_S="$OPTARG" ;;
    *) echo "Использование: $0 [-h ssh_alias] [-d duration_s]"; exit 1 ;;
    esac
done

REPORT="$(pwd)/cases_report.txt"
exec > >(tee "$REPORT") 2>&1

echo "============================================================"
echo "  Прогон case-программ через sysmond"
echo "  Дата:                    $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Целевой хост:            $QNX_HOST"
echo "  Длительность каждого:    $DURATION_S сек"
echo "============================================================"
echo

if ! ssh -o ConnectTimeout=5 "$QNX_HOST" 'echo OK' >/dev/null 2>&1; then
    echo "ОШИБКА: нет связи с $QNX_HOST"
    exit 1
fi

ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null; sleep 1" >/dev/null
ssh "$QNX_HOST" "/usr/bin/sysmond -p 100" >/dev/null
sleep 2
echo "sysmond запущен с периодом 100 мс."
echo

declare -A CASES=(
    ["case_cpu_burner"]="$DURATION_S"
    ["case_periodic_rt"]="50 5 $DURATION_S"
    ["case_ipc_pingpong"]="$DURATION_S"
    ["case_mutex_contention"]="4 $DURATION_S"
)

ORDER=(case_cpu_burner case_periodic_rt case_ipc_pingpong case_mutex_contention)

get_pid_states() {
    local pid="$1"
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli snapshot 2>/dev/null" \
        | awk -v pid="$pid" '$1 == pid'
}

analyze_history() {
    local pid="$1"
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli history -p $pid -n 5000 2>/dev/null" \
        | awk -v pid="$pid" '
            $2 == pid && $5 == "RUN" { run++ }
            $2 == pid && $5 == "RDY" { rdy++ }
            $2 == pid && $5 == "BLK" { blk++ }
            $2 == pid && $5 == "BLK" {
                for (i = NF; i >= 1; i--) {
                    if ($i ~ /^\[/) {
                        gsub(/[\[\]]/, "", $i)
                        blk_types[$i]++
                        break
                    }
                }
            }
            END {
                total = run + rdy + blk
                if (total == 0) { print "нет данных"; exit }
                printf "RUN=%.1f%% RDY=%.1f%% BLK=%.1f%% (выборок: %d)\n",
                    100.0 * run / total,
                    100.0 * rdy / total,
                    100.0 * blk / total,
                    total
                printf "  причины блокировки:"
                for (t in blk_types) {
                    printf " %s=%.1f%%", t, 100.0 * blk_types[t] / total
                }
                print ""
            }'
}

for case_name in "${ORDER[@]}"; do
    args="${CASES[$case_name]}"

    echo "============================================================"
    echo "  $case_name (аргументы: $args)"
    echo "============================================================"

    ssh "$QNX_HOST" "nohup /usr/bin/$case_name $args > /tmp/${case_name}.log 2>&1 &" &
    SSH_PID=$!
    sleep 2

    pid=$(ssh "$QNX_HOST" "pidin -F '%a %N' 2>/dev/null \
        | awk -v name=\"$case_name\" '{n=\$2; sub(/.*\\//, \"\", n); if(index(name,n)>0 && length(n)>4){print \$1; exit}}'")

    if [ -z "$pid" ]; then
        echo "  WARN: не удалось найти PID case-программы"
        wait $SSH_PID 2>/dev/null
        continue
    fi

    echo "  PID = $pid"
    echo "  -- Снимок в начале --"
    get_pid_states "$pid" | head -10

    sleep $((DURATION_S - 6))

    echo "  -- Снимок в конце --"
    get_pid_states "$pid" | head -10

    echo "  -- Анализ истории --"
    analyze_history "$pid"

    wait $SSH_PID 2>/dev/null

    echo "  -- Лог case-программы --"
    ssh "$QNX_HOST" "tail -3 /tmp/${case_name}.log 2>/dev/null"
    echo

    sleep 2
done

ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null" >/dev/null

echo "============================================================"
echo "  ИТОГ"
echo "============================================================"
echo "Отчёт сохранён в: $REPORT"
