#!/bin/bash
#
# diag_full.sh — Полная диагностика sysmond: сборка, деплой, запуск, snapshot
#
# Запускается на Ubuntu. Выполняет цепочку шагов:
#   1. Активирует окружение КПДА (если ещё не активно)
#   2. Пересобирает проект
#   3. Сверяет хэши локального бинарника и на Нейтрино
#   4. Деплоит на Нейтрино
#   5. Убивает старый sysmond, удаляет старый core
#   6. Запускает sysmond с логом в /tmp/sysmond.log
#   7. Проверяет регистрацию /dev/sysmon
#   8. Делает snapshot
#   9. Собирает финальный отчёт
#
# Вывод — потоковый, дублируется в файл diag_report.txt
#
# Использование:
#   chmod +x diag_full.sh
#   ./diag_full.sh
#   ./diag_full.sh ~/diplom/src       # указать путь к проекту
#

set -u   # ошибка при использовании unset переменных

# ---- Параметры ------------------------------------------------------
SRC_DIR="${1:-$HOME/diplom/src}"
QNX_HOST_ALIAS="${2:-qnx}"
REPORT="$(pwd)/diag_report.txt"

# ---- Дублируем вывод в файл -----------------------------------------
exec > >(tee "$REPORT") 2>&1

# ---- Утилита: красивый заголовок шага -------------------------------
step() {
    echo
    echo "============================================================"
    echo "  $1"
    echo "============================================================"
}

# ---- Утилита: обернуть SSH-команду с заголовком ---------------------
qnx_run() {
    local label="$1"
    local cmd="$2"
    echo "--- $label ---"
    ssh -o ConnectTimeout=5 "$QNX_HOST_ALIAS" "$cmd" 2>&1 || \
        echo "  (выход: $?)"
    echo
}

# =====================================================================
echo "Отчёт диагностики sysmond"
echo "Дата:    $(date '+%Y-%m-%d %H:%M:%S')"
echo "Хост:    $(hostname)"
echo "Проект:  $SRC_DIR"
echo "Нейтрино: $QNX_HOST_ALIAS"

# ---------------------------------------------------------------------
step "1. Окружение КПДА"
echo "QNX_HOST   = ${QNX_HOST:-(не задано)}"
echo "QNX_TARGET = ${QNX_TARGET:-(не задано)}"
echo
if [ -z "${QNX_HOST:-}" ] || [ -z "${QNX_TARGET:-}" ]; then
    echo "Активирую окружение КПДА вручную..."
    export QNX_HOST=/opt/kpda2021a/host/linux64/x86_64
    export QNX_TARGET=/opt/kpda2021a/target/neutrino
    export PATH=$QNX_HOST/usr/bin:$PATH
    echo "QNX_HOST   = $QNX_HOST"
    echo "QNX_TARGET = $QNX_TARGET"
fi
echo "qcc        = $(which qcc 2>/dev/null || echo НЕТ)"

# ---------------------------------------------------------------------
step "2. Проверка проекта"
if [ ! -d "$SRC_DIR" ]; then
    echo "ОШИБКА: каталог $SRC_DIR не существует"
    exit 1
fi
cd "$SRC_DIR"
echo "Каталог: $(pwd)"
echo
echo "Свежесть исходных файлов:"
ls -la common/sysmon_protocol.h \
       daemon/main.c            \
       daemon/collector.c       \
       daemon/resmgr_handler.c  \
       Makefile                 2>&1

# ---------------------------------------------------------------------
step "3. Чистая пересборка"
make clean 2>&1
echo "--- make ARCH=x86 DEBUG=1 ---"
if ! make ARCH=x86 DEBUG=1 2>&1; then
    echo
    echo "ОШИБКА: сборка провалилась"
    exit 1
fi
echo
echo "Артефакты:"
ls -la sysmond sysmon_cli 2>&1
echo
LOCAL_HASH=$(md5sum sysmond 2>&1 | awk '{print $1}')
echo "MD5 локального sysmond: $LOCAL_HASH"

# ---------------------------------------------------------------------
step "4. Проверка SSH-соединения"
if ! ssh -o ConnectTimeout=5 "$QNX_HOST_ALIAS" 'echo OK' 2>&1; then
    echo "ОШИБКА: нет связи с $QNX_HOST_ALIAS"
    exit 1
fi

# ---------------------------------------------------------------------
step "5. Очистка состояния на Нейтрино"
qnx_run "Убиваем старый sysmond"      "slay -f sysmond 2>&1; sleep 1; echo done"
qnx_run "Проверяем что упал"          "pidin | grep sysmond || echo 'sysmond отсутствует — OK'"
qnx_run "Удаляем старые core-дампы"   "rm -f /var/dumps/sysmond.core /tmp/sysmond.log; ls /var/dumps/ /tmp/sysmond.log 2>&1 | head"
qnx_run "Проверяем что /dev/sysmon ушёл" "ls -la /dev/sysmon 2>&1"

# ---------------------------------------------------------------------
step "6. Деплой нового бинарника"
echo "--- scp ---"
scp sysmond sysmon_cli "$QNX_HOST_ALIAS:/usr/bin/" 2>&1
ssh "$QNX_HOST_ALIAS" 'chmod +x /usr/bin/sysmond /usr/bin/sysmon_cli' 2>&1
echo
qnx_run "Сверка хэшей"  "md5sum /usr/bin/sysmond"
echo "Ожидаемый хэш:    $LOCAL_HASH"
REMOTE_HASH=$(ssh "$QNX_HOST_ALIAS" 'md5sum /usr/bin/sysmond' 2>&1 | awk '{print $1}')
if [ "$LOCAL_HASH" = "$REMOTE_HASH" ]; then
    echo "Хэши СОВПАДАЮТ ✓"
else
    echo "ВНИМАНИЕ: хэши РАЗНЫЕ ($LOCAL_HASH vs $REMOTE_HASH)"
fi

# ---------------------------------------------------------------------
step "7. Запуск sysmond"
qnx_run "Запуск (foreground в фоне с redirect в /tmp/sysmond.log)" \
    "nohup /usr/bin/sysmond -f -p 1000 > /tmp/sysmond.log 2>&1 &
     sleep 2
     echo done"

qnx_run "Лог запуска (что вывел sysmond)" "cat /tmp/sysmond.log 2>&1"

qnx_run "Процесс sysmond"           "pidin | grep sysmond || echo 'sysmond НЕ запущен'"

qnx_run "Проверка /dev/sysmon"      "ls -la /dev/sysmon 2>&1"

qnx_run "Полный список /dev"         "ls /dev/ | head -50"

# ---------------------------------------------------------------------
step "8. Запрос snapshot"
qnx_run "snapshot" "/usr/bin/sysmon_cli snapshot 2>&1"

qnx_run "stats"    "/usr/bin/sysmon_cli stats 2>&1"

qnx_run "config"   "/usr/bin/sysmon_cli config 2>&1"

# ---------------------------------------------------------------------
step "9. Финальное состояние"
SYSMOND_ALIVE=$(ssh "$QNX_HOST_ALIAS" 'pidin | grep -c sysmond' 2>/dev/null)
qnx_run "sysmond живой?"      "pidin | grep sysmond || echo 'sysmond УПАЛ'"
qnx_run "Есть ли core?"        "ls -la /var/dumps/ 2>&1"
qnx_run "Финальный лог"        "cat /tmp/sysmond.log 2>&1 | tail -20"

# ---------------------------------------------------------------------
step "10. Анализ core-файла (если есть)"

# Проверяем — есть ли core на Нейтрино
if ssh "$QNX_HOST_ALIAS" 'test -f /var/dumps/sysmond.core' 2>/dev/null; then
    CRASH_DIR="$(pwd)/crash-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$CRASH_DIR"
    echo "Создан каталог для артефактов: $CRASH_DIR"
    echo

    echo "--- Копирую core-файл с Нейтрино ---"
    scp "$QNX_HOST_ALIAS:/var/dumps/sysmond.core" "$CRASH_DIR/" 2>&1
    echo

    # ВАЖНО: копируем бинарник С Нейтрино (а не локальный) —
    # для gdb символы и адреса должны точно совпадать с тем,
    # что упало. Если пересоберём — локальный отличается.
    echo "--- Копирую упавший бинарник с Нейтрино ---"
    scp "$QNX_HOST_ALIAS:/usr/bin/sysmond" "$CRASH_DIR/sysmond" 2>&1
    echo

    echo "--- Сверка: тот ли это бинарник, что был задеплоен ---"
    LOCAL_DEPLOYED_HASH=$(md5sum "$CRASH_DIR/sysmond" 2>/dev/null | awk '{print $1}')
    echo "Хэш бинарника, что упал:   $LOCAL_DEPLOYED_HASH"
    echo "Хэш текущего билда:        $LOCAL_HASH"
    if [ "$LOCAL_DEPLOYED_HASH" = "$LOCAL_HASH" ]; then
        echo "Совпадают — gdb-символы будут актуальны"
    else
        echo "ВНИМАНИЕ: разные хэши! gdb будет работать,"
        echo "но локальные исходники могут отличаться от тех, что упали."
    fi
    echo

    # Найти gdb для целевой архитектуры
    GDB=""
    for candidate in \
        "$QNX_HOST/usr/bin/i486-pc-nto-qnx6.5.0-gdb" \
        "$QNX_HOST/usr/bin/ntox86-gdb" \
        "$QNX_HOST/usr/bin/gdb"; do
        if [ -x "$candidate" ]; then
            GDB="$candidate"
            break
        fi
    done

    if [ -z "$GDB" ]; then
        echo "--- gdb не найден ---"
        echo "Каталог $QNX_HOST/usr/bin содержит:"
        ls "$QNX_HOST/usr/bin/" 2>/dev/null | grep -i gdb | sed "s/^/  /"
    else
        echo "--- gdb: $GDB ---"
        echo

        # Команды gdb выполняем через batch-режим
        cat > "$CRASH_DIR/gdb_cmds.txt" << 'GDB_EOF'
set pagination off
set print pretty on

echo \n=== thread apply all bt ===\n
thread apply all bt

echo \n=== info threads ===\n
info threads

echo \n=== thread 1 ===\n
thread 1
echo \n=== bt full ===\n
bt full

echo \n=== info registers ===\n
info registers

echo \n=== info symbol pc ===\n
info symbol $pc

echo \n=== disassemble pc-32, pc+32 ===\n
disassemble $pc-32, $pc+32

echo \n=== shared libraries ===\n
info sharedlibrary

quit
GDB_EOF

        echo "--- gdb output ---"
        "$GDB" "$CRASH_DIR/sysmond" "$CRASH_DIR/sysmond.core" \
            -batch -x "$CRASH_DIR/gdb_cmds.txt" 2>&1
        echo
        echo "(полные артефакты сохранены в $CRASH_DIR)"
    fi
else
    echo "Core-файла нет — sysmond или не падал, или дамп не настроен."
fi

# ---------------------------------------------------------------------
step "ИТОГ"
echo "Полный отчёт сохранён в: $REPORT"
echo
if [ "${CRASH_DIR:-}" != "" ]; then
    echo "Артефакты крэша:        $CRASH_DIR"
    echo "  - sysmond.core    (дамп памяти)"
    echo "  - sysmond         (упавший бинарник для gdb-символов)"
    echo "  - gdb_cmds.txt    (команды для gdb)"
fi
echo
echo "Что отправлять для разбора:"
echo "  1. Содержимое $REPORT"
[ "${CRASH_DIR:-}" != "" ] && echo "  2. Каталог $CRASH_DIR (для дальнейшего gdb)"
echo
echo "  cat $REPORT | xclip -selection clipboard   # скопировать в буфер"
echo "  или просто открой файл и пришли"
