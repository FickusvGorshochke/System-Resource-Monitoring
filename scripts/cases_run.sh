#!/bin/bash
#
# cases_run.sh — прогон case-программ через sysmond и сбор метрик.
#
# Для каждой case-программы:
#   1. Перезапускаем sysmond с периодом 100 мс (10 Гц для деталей)
#   2. Запускаем case-программу с таймаутом DURATION_S сек
#   3. После старта ждём 2 сек, делаем snapshot и сохраняем
#   4. Через DURATION_S сек делаем снова snapshot
#   5. Получаем sysmon_cli history по PID case-программы
#   6. Собираем агрегированные метрики (доли в RUN/READY/BLOCKED, причины блок.)
#
# Результат — файл cases_report.txt со сводной таблицей.
#
# Запуск:
#   ./cases_run.sh                  # стандартный, 30 сек на case
#   ./cases_run.sh -d 60            # 60 сек на case
#   ./cases_run.sh -h qnx2          # другой SSH alias

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

# ---- Чистый старт sysmond с периодом 100 мс ------------------------
ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null; sleep 1" >/dev/null
ssh "$QNX_HOST" "/usr/bin/sysmond -p 100" >/dev/null
sleep 2
echo "sysmond запущен с периодом 100 мс."
echo

# ---- Список case-программ ------------------------------------------
declare -A CASES=(
    ["case_cpu_burner"]="$DURATION_S"
    ["case_periodic_rt"]="50 5 $DURATION_S"
    ["case_ipc_pingpong"]="$DURATION_S"
    ["case_mutex_contention"]="4 $DURATION_S"
)

ORDER=(case_cpu_burner case_periodic_rt case_ipc_pingpong case_mutex_contention)

# ---- Хелпер: запросить snapshot и достать состояния PID ------------
get_pid_states() {
    local pid="$1"
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli snapshot 2>/dev/null" \
        | awk -v pid="$pid" '$1 == pid'
}

# ---- Хелпер: подсчёт долей по состояниям из history ----------------
# Использует sysmon_cli history -p PID, считает RUN/READY/BLK
analyze_history() {
    local pid="$1"
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli history -p $pid -n 5000 2>/dev/null" \
        | awk '
            $4 == "RUN" { run++ }
            $4 == "RDY" { rdy++ }
            $4 == "BLK" { blk++ }
            $4 == "BLK" {
                # извлекаем причину блокировки [TYPE]
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

# ---- Основной цикл -------------------------------------------------
for case_name in "${ORDER[@]}"; do
    args="${CASES[$case_name]}"

    echo "============================================================"
    echo "  $case_name (аргументы: $args)"
    echo "============================================================"

    # Запускаем case-программу в фоне
    ssh "$QNX_HOST" "/usr/bin/$case_name $args > /tmp/${case_name}.log 2>&1 &" &
    SSH_PID=$!
    sleep 2

    # Ищем PID case-программы
    pid=$(ssh "$QNX_HOST" "pidin -F '%a %N' 2>/dev/null \
        | awk '\$2 == \"$case_name\" {print \$1; exit}'")

    if [ -z "$pid" ]; then
        echo "  WARN: не удалось найти PID case-программы"
        wait $SSH_PID 2>/dev/null
        continue
    fi

    echo "  PID = $pid"
    echo "  -- Снимок в начале --"
    get_pid_states "$pid" | head -10

    # Ждём DURATION_S - 2 сек (мы уже потратили 2 на старт)
    sleep $((DURATION_S - 2))

    echo "  -- Снимок в конце --"
    get_pid_states "$pid" | head -10

    echo "  -- Анализ истории --"
    analyze_history "$pid"

    # Дожидаемся завершения case-программы (сама выйдет по таймауту)
    wait $SSH_PID 2>/dev/null

    # Лог
    echo "  -- Лог case-программы --"
    ssh "$QNX_HOST" "tail -3 /tmp/${case_name}.log 2>/dev/null"
    echo

    # Отдых между прогонами (чтобы sysmond успел вернуться к фоновой нагрузке)
    sleep 2
done

# ---- Очистка -------------------------------------------------------
ssh "$QNX_HOST" "slay -f sysmond 2>/dev/null" >/dev/null

echo "============================================================"
echo "  ИТОГ"
echo "============================================================"
echo "Отчёт сохранён в: $REPORT"
echo
echo "Ожидания (для главы 5 ВКР):"
echo "  case_cpu_burner       — RUN ≈ 95-100%, BLK малый"
echo "  case_periodic_rt      — RUN ≈ 10%, BLK ≈ 90% (NANOSLEEP)"
echo "  case_ipc_pingpong     — BLK ≈ 99% (RECEIVE/REPLY)"
echo "  case_mutex_contention — RUN+RDY+BLK(MUTEX) распределены равномерно"
