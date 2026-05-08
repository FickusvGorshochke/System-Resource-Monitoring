#!/bin/bash
#
# bench.sh — измерение CPU overhead сервиса sysmond.
#
# Метрика — sutime_ns самих потоков sysmond, считанная через
# sysmon_cli cpuns (наносекундная точность от ядра procfs).
#
# Особенности:
#  * каждая конфигурация замеряется N_RUNS раз, печатается медиана;
#  * длительность одного замера автоматически масштабируется так,
#    чтобы в окно поместилось ≥ 10 опросов (при низких частотах
#    окно становится длиннее, чем DURATION_S_MIN);
#  * cpuns возвращает timestamp САМОГО опроса (а не времени запроса) —
#    wall_ns и cpu_ns берутся из одной временной шкалы.
#
# Использование:
#   ./bench.sh                           # стандартный прогон
#   ./bench.sh -d 30 -n 5                # 30 сек × 5 повторений
#   ./bench.sh -p 100,500,1000           # только указанные периоды

set -u

QNX_HOST="qnx"
DURATION_S_MIN=20
N_RUNS=3
PERIODS=(100 250 500 1000 2000 5000)

while getopts "h:d:n:p:" opt; do
    case "$opt" in
    h) QNX_HOST="$OPTARG" ;;
    d) DURATION_S_MIN="$OPTARG" ;;
    n) N_RUNS="$OPTARG" ;;
    p) IFS=',' read -r -a PERIODS <<< "$OPTARG" ;;
    *)
        echo "Использование: $0 [-h ssh_alias] [-d duration_s_min] [-n n_runs] [-p ms1,ms2,...]"
        exit 1
        ;;
    esac
done

REPORT="$(pwd)/bench_report.txt"
exec > >(tee "$REPORT") 2>&1

echo "============================================================"
echo "  Бенчмарк sysmond — overhead на CPU"
echo "  Дата:                   $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Целевой хост:           $QNX_HOST"
echo "  Минимум на конфиг.:     $DURATION_S_MIN сек"
echo "  Повторов на конфиг.:    $N_RUNS"
echo "  Периоды опроса (мс):    ${PERIODS[*]}"
echo "  Источник CPU-метрики:   sysmon_cli cpuns"
echo "============================================================"
echo

if ! ssh -o ConnectTimeout=5 "$QNX_HOST" 'echo OK' >/dev/null 2>&1; then
    echo "ОШИБКА: нет связи с $QNX_HOST"
    exit 1
fi

get_sysmond_pid() {
    ssh "$QNX_HOST" "pidin -F '%a %N' 2>/dev/null \
        | awk '\$2 == \"sysmond\" {print \$1; exit}'"
}

# Возвращает три числа: poll_timestamp_ns, sum_sutime_ns, thread_count
get_cpu_data() {
    local pid="$1"
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli cpuns $pid 2>/dev/null"
}

get_threads_total() {
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli stats 2>/dev/null" \
        | awk -F: '/Потоков в последнем снимке/ { gsub(/ /,"",$2); print $2 }'
}

# median по списку чисел в %.4f формате
median() {
    printf '%s\n' "$@" | sort -g | awk '
        { v[NR]=$0 }
        END {
            n = NR
            if (n == 0) { print "0"; exit }
            if (n % 2) printf "%s", v[(n+1)/2]
            else       printf "%.4f", (v[n/2] + v[n/2+1]) / 2
        }'
}

printf "%-10s %-10s %-9s %-12s %-12s %-12s %-10s\n" \
    "Период" "Частота" "Потоков" "CPU% (med)" "CPU% (min)" "CPU% (max)" "Окно(с)"
printf "%-10s %-10s %-9s %-12s %-12s %-12s %-10s\n" \
    "------" "-------" "-------" "----------" "----------" "----------" "-------"

for period in "${PERIODS[@]}"; do
    # Окно ≥ DURATION_S_MIN, но не меньше чем 10 периодов опроса
    win_s=$(awk -v d="$DURATION_S_MIN" -v p="$period" '
        BEGIN {
            min_for_polls = 10 * p / 1000
            if (min_for_polls < d) print d
            else printf "%d", min_for_polls + 1
        }')

    runs=()
    for ((run=1; run<=N_RUNS; run++)); do
        ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null; sleep 1" >/dev/null
        ssh "$QNX_HOST" "/usr/bin/sysmond -p $period" >/dev/null

        # Дать sysmond сделать минимум 2 опроса до начала измерения
        warmup=$(awk -v p="$period" 'BEGIN { printf "%.1f", 2 * p / 1000.0 + 1.0 }')
        sleep "$warmup"

        pid=$(get_sysmond_pid)
        if [ -z "$pid" ]; then
            runs+=("FAIL_PID")
            continue
        fi

        data_b=$(get_cpu_data "$pid")
        sleep "$win_s"
        data_a=$(get_cpu_data "$pid")

        if [ -z "$data_b" ] || [ -z "$data_a" ]; then
            runs+=("FAIL_DATA")
            continue
        fi

        cpu_pct=$(awk -v before="$data_b" -v after="$data_a" '
            BEGIN {
                split(before, b, " ")
                split(after,  a, " ")
                wall_ns = a[1] - b[1]
                cpu_ns  = a[2] - b[2]
                if (wall_ns <= 0) { print "FAIL_WALL"; exit }
                printf "%.4f", cpu_ns / wall_ns * 100.0
            }')

        runs+=("$cpu_pct")
    done

    # Числовые результаты для статистики
    valid_runs=()
    for v in "${runs[@]}"; do
        case "$v" in
            FAIL_*) ;;
            *) valid_runs+=("$v") ;;
        esac
    done

    threads=$(get_threads_total)
    [ -z "$threads" ] && threads="?"
    freq=$(awk -v p="$period" 'BEGIN { printf "%.1f", 1000.0 / p }')

    if [ ${#valid_runs[@]} -eq 0 ]; then
        printf "%-10s %-10s %-9s %-12s %-12s %-12s %-10s\n" \
            "${period}мс" "${freq}Гц" "$threads" "FAIL" "—" "—" "$win_s"
        echo "  └─ детали: ${runs[*]}"
        continue
    fi

    med=$(median "${valid_runs[@]}")
    minv=$(printf '%s\n' "${valid_runs[@]}" | sort -g | head -1)
    maxv=$(printf '%s\n' "${valid_runs[@]}" | sort -g | tail -1)

    printf "%-10s %-10s %-9s %-12s %-12s %-12s %-10s\n" \
        "${period}мс" "${freq}Гц" "$threads" \
        "${med}%" "${minv}%" "${maxv}%" "$win_s"
done

ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null" >/dev/null

echo
echo "============================================================"
echo "  ИТОГ"
echo "============================================================"
echo "Отчёт сохранён в: $REPORT"
echo
echo "Колонки:"
echo "  Окно(с)    — длительность одного замера (≥ 10 периодов опроса)"
echo "  CPU% (med) — медиана по N_RUNS = $N_RUNS прогонам"
echo "  min / max  — разброс между прогонами"
echo
echo "Целевые цифры из методики:"
echo "  Период 1000 мс (1 Гц):  типично < 1% CPU"
echo "  Период 100 мс  (10 Гц): типично 1-3% CPU"
