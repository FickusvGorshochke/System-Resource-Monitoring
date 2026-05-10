#!/bin/bash
#
# test_persistence.sh — тестирование механизма сохранения/загрузки
# истории кольцевого буфера sysmond.
#
# Сценарий:
#   1. Чистый старт sysmond.
#   2. Накопление истории (DURATION_S сек).
#   3. Запоминание счётчика записей.
#   4. Отправка SIGTERM (graceful shutdown).
#   5. Проверка появления файла дампа /var/log/sysmond_history.bin.
#   6. Проверка валидности заголовка файла (магическое число SYSMOND).
#   7. Перезапуск sysmond.
#   8. Проверка, что история подгрузилась (по статистике и логу).
#
# Использование:
#   ./test_persistence.sh                  # стандартный прогон
#   ./test_persistence.sh -d 60            # 60 сек на накопление
#   ./test_persistence.sh -h qnx2          # другой SSH alias

set -u

QNX_HOST="qnx"
DURATION_S=30
DUMP_PATH="/var/log/sysmond_history.bin"

while getopts "h:d:l:" opt; do
    case "$opt" in
    h) QNX_HOST="$OPTARG" ;;
    d) DURATION_S="$OPTARG" ;;
    l) DUMP_PATH="$OPTARG" ;;
    *)
        echo "Использование: $0 [-h ssh_alias] [-d duration_s] [-l dump_path]"
        exit 1
        ;;
    esac
done

REPORT="$(pwd)/persistence_report.txt"
exec > >(tee "$REPORT") 2>&1

echo "============================================================"
echo "  Тест персистенса sysmond"
echo "  Дата:                $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Целевой хост:        $QNX_HOST"
echo "  Длительность сбора:  $DURATION_S сек"
echo "  Путь к дампу:        $DUMP_PATH"
echo "============================================================"
echo

# ---- Проверка SSH ---------------------------------------------------
if ! ssh -o ConnectTimeout=5 "$QNX_HOST" 'echo OK' >/dev/null 2>&1; then
    echo "ОШИБКА: нет связи с $QNX_HOST"
    exit 1
fi

step() {
    echo
    echo "------------------------------------------------------------"
    echo "  $1"
    echo "------------------------------------------------------------"
}

get_sysmond_pid() {
    ssh "$QNX_HOST" "pidin -F '%a %N' 2>/dev/null \
        | awk '\$2 == \"sysmond\" {print \$1; exit}'"
}

get_record_count() {
    ssh "$QNX_HOST" "/usr/bin/sysmon_cli stats 2>/dev/null" \
        | awk -F: '/Всего записей/ { gsub(/[ \t]/,"",$2); print $2 }'
}

# ---- 1. Очистка состояния -------------------------------------------
step "1. Очистка предыдущего состояния"
echo "Останавливаем sysmond (если был запущен)..."
ssh "$QNX_HOST" "kill -TERM \$(pidin -F '%a %N' 2>/dev/null \
    | awk '\$2 == \"sysmond\" {print \$1; exit}') 2>/dev/null; sleep 2"

echo "Удаляем старый дамп..."
ssh "$QNX_HOST" "rm -f $DUMP_PATH"

echo "Проверка состояния перед стартом:"
ssh "$QNX_HOST" "ls $DUMP_PATH 2>&1 | head -3"
ssh "$QNX_HOST" "pidin | grep sysmond || echo 'sysmond не запущен'"

# ---- 2. Чистый запуск -----------------------------------------------
step "2. Запуск sysmond с опцией -N (не загружать прошлый дамп)"
ssh "$QNX_HOST" "/usr/bin/sysmond -p 1000 -N"
sleep 2

PID_BEFORE=$(get_sysmond_pid)
if [ -z "$PID_BEFORE" ]; then
    echo "ОШИБКА: sysmond не запустился"
    exit 1
fi
echo "sysmond запущен, PID = $PID_BEFORE"

# ---- 3. Накопление истории ------------------------------------------
step "3. Накопление истории ($DURATION_S сек)"
sleep "$DURATION_S"

COUNT_BEFORE=$(get_record_count)
[ -z "$COUNT_BEFORE" ] && COUNT_BEFORE=0
echo "Записей в буфере перед остановкой: $COUNT_BEFORE"

ssh "$QNX_HOST" "/usr/bin/sysmon_cli stats 2>/dev/null"

# ---- 4. Graceful shutdown через SIGTERM -----------------------------
step "4. Корректное завершение через SIGTERM"
echo "Посылаем SIGTERM в PID $PID_BEFORE..."
ssh "$QNX_HOST" "kill -TERM $PID_BEFORE"
sleep 2

if [ -n "$(get_sysmond_pid)" ]; then
    echo "ВНИМАНИЕ: sysmond ещё жив через 2 сек, повторяем..."
    sleep 2
fi

if [ -n "$(get_sysmond_pid)" ]; then
    echo "ОШИБКА: sysmond не реагирует на SIGTERM"
    exit 1
fi
echo "sysmond завершён."

# ---- 5. Проверка файла дампа ----------------------------------------
step "5. Проверка появления файла дампа"
DUMP_LS=$(ssh "$QNX_HOST" "ls -la $DUMP_PATH 2>&1")
echo "$DUMP_LS"

if echo "$DUMP_LS" | grep -qi "no such file"; then
    echo
    echo "✗ ПРОВАЛ: файл дампа не создан"
    echo
    echo "Возможные причины:"
    echo "  - sysmond собран без поддержки дампа"
    echo "  - SIGTERM не дошёл (kill: invalid signal?)"
    echo "  - Ошибка записи в $DUMP_PATH (нет каталога?)"
    echo
    echo "Лог sysmond:"
    ssh "$QNX_HOST" "cat /var/log/sysmond.log 2>/dev/null | tail -15"
    exit 1
fi

DUMP_SIZE=$(echo "$DUMP_LS" | awk '{print $5}')
echo "Размер дампа: $DUMP_SIZE байт"

# ---- 6. Проверка заголовка ------------------------------------------
step "6. Проверка магического числа SYSMOND в заголовке"
MAGIC=$(ssh "$QNX_HOST" "od -An -c -N 8 $DUMP_PATH" | tr -d ' \\\\0' | head -c 7)
echo "Прочитано: '$MAGIC'"

if [ "$MAGIC" = "SYSMOND" ]; then
    echo "✓ Магическое число корректное."
else
    echo "✗ ВНИМАНИЕ: магическое число НЕ совпадает с 'SYSMOND'"
    echo "Сырой дамп первых 24 байт:"
    ssh "$QNX_HOST" "od -An -t x1 -N 24 $DUMP_PATH"
fi

echo
echo "Заголовок (24 байта в hex+ascii):"
ssh "$QNX_HOST" "od -A x -t x1z -N 24 $DUMP_PATH"

# Расчёт ожидаемого числа записей: (size - 24) / 56
RECORDS_IN_FILE=$(awk -v s="$DUMP_SIZE" 'BEGIN { print (s - 24) / 56 }')
echo "Записей в файле (по размеру): $RECORDS_IN_FILE"

# ---- 7. Перезапуск с подгрузкой -------------------------------------
step "7. Перезапуск sysmond (без -N — должен подгрузить дамп)"
ssh "$QNX_HOST" "rm -f /tmp/sysmond_persist.log"
ssh "$QNX_HOST" "/usr/bin/sysmond -f -p 1000 > /tmp/sysmond_persist.log 2>&1 &"
sleep 3

PID_AFTER=$(get_sysmond_pid)
if [ -z "$PID_AFTER" ]; then
    echo "ОШИБКА: sysmond не запустился после перезагрузки"
    ssh "$QNX_HOST" "cat /tmp/sysmond_persist.log 2>/dev/null"
    exit 1
fi
echo "sysmond запущен, PID = $PID_AFTER"

# ---- 8. Проверка лога подгрузки -------------------------------------
step "8. Проверка лога — есть ли строка 'Подгружено'"
LOG=$(ssh "$QNX_HOST" "cat /tmp/sysmond_persist.log 2>/dev/null")
echo "$LOG"
echo

if echo "$LOG" | grep -qE "Подгружено [0-9]+ записей"; then
    LOADED=$(echo "$LOG" | grep -oE "Подгружено [0-9]+" | head -1 | awk '{print $2}')
    echo "✓ ПОДТВЕРЖДЕНО: подгружено $LOADED записей из дампа"
else
    echo "✗ Строка о подгрузке не найдена."
fi

# ---- 9. Проверка через stats ----------------------------------------
step "9. Состояние буфера после перезагрузки"
sleep 2
ssh "$QNX_HOST" "/usr/bin/sysmon_cli stats 2>/dev/null"

# ---- 10. Очистка ----------------------------------------------------
step "10. Финальная очистка"
ssh "$QNX_HOST" "kill -TERM $PID_AFTER 2>/dev/null; sleep 1"

echo
echo "============================================================"
echo "  ИТОГ"
echo "============================================================"
echo "Записей до остановки:    $COUNT_BEFORE"
echo "Записей в файле дампа:   $RECORDS_IN_FILE"
[ -n "${LOADED:-}" ] && echo "Записей подгружено:      $LOADED"
echo
echo "Отчёт сохранён в: $REPORT"
