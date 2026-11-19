#!/usr/bin/env bash
# ============================================================================
# 在已连接的 Android 设备/模拟器上跑全部能力探针，并汇总 OK/NG 数。
#
# 前提：已装 com.leno.app（android/build_android.sh 产物），设备已连上 adb。
# 用法： bash android/run_probes.sh              # 跑全部
#        bash android/run_probes.sh c6_net       # 只跑一个
#
# 为什么用 logcat 收结果而不是截图：探针一次输出几十行，截图要逐字认；
#   leno_jni.c 里已经把 VM 的每一行 stdout 镜像到 logcat（tag=leno_jni）。
# ============================================================================
set -u
ADB="${ADB:-adb}"
PKG=com.leno.app
ACT=.MainActivity
ACT_SDL=.SdlActivity   # s* 探针要开窗 ⇒ 必须跑在 SDL 宿主里（见 SdlActivity.java）
ASSETS="$(cd "$(dirname "$0")" && pwd)/app/assets"

if [ $# -gt 0 ]; then
    LIST="$*"
else
    # c* = 纯文本宿主探针（MainActivity）；s* = SDL 宿主探针（SdlActivity）
    LIST=$(cd "$ASSETS" && ls -1 c*.leno s*.leno 2>/dev/null | sed 's/\.leno$//' | sort -V)
fi

# 清空 logcat 并**确认清干净**。
# 为什么不是一句 `logcat -c`：实测它在连续调用时偶尔会静默失败（错误被 2>/dev/null 吞了），
#   上一轮的输出留在缓冲里 ⇒ 下一轮的 grep '^OK ' 把历史一起算进去，
#   汇总数会虚高（曾出现 c5_thread 报 44 条、实际只有 8 条）。
#   所以这里清完必须回读验证，不干净就再来一次。
clear_log() {
    local i n
    for i in 1 2 3 4 5; do
        "$ADB" logcat -c 2>/dev/null || true
        # ⚠ `logcat -c` 是**异步**的：命令返回时 logd 可能还没真正清完，
        #   立刻回读必然读到旧数据 ⇒ 误判"没清干净" ⇒ 重试永远失败。
        #   这个 sleep 不是凑数，是清空动作本身的收敛等待。
        sleep 0.5
        # 只数真正的日志行；"--------- beginning of main/system/kernel" 是分隔行，不算内容
        n=$("$ADB" logcat -d -s leno_jni 2>/dev/null | grep -cv '^---------')
        [ "${n:-0}" -le 0 ] && return 0
    done
    echo "  ⚠ logcat 未能清空（仍有 $n 行），本次统计可能虚高" >&2
    return 1
}

printf '%-12s %6s %6s  %s\n' 探针 OK NG 备注
printf '%-12s %6s %6s  %s\n' ------------ ------ ------ ------------------------------

total_ok=0; total_ng=0
for p in $LIST; do
    clear_log
    act="$ACT"
    case "$p" in s*) act="$ACT_SDL";; esac
    "$ADB" shell "am force-stop $PKG" >/dev/null 2>&1
    "$ADB" shell "am start -n $PKG/$act --es asset $p.lenb" >/dev/null 2>&1

    # 轮询等探针自己打出 `== cX 结果: ... ==`，而不是死等固定秒数。
    # 为什么：固定 sleep 会在"探针已经跑完、但最后几条还堵在 logcat 缓冲里"时漏计
    #   —— 实测 c6_net 在 10s 窗口下只抓到 3 条，实际是 6 条，汇总数于是偏低。
    #   探针失败（VM 报错、没有结果行）时仍等满上限，不会卡死。
    limit=$(( ${PROBE_WAIT:-12} * 2 ))      # 单位：半秒
    half=0
    while [ "$half" -lt "$limit" ]; do
        if "$ADB" logcat -d -s leno_jni 2>/dev/null | grep -aq '结果:'; then break; fi
        sleep 0.5
        half=$((half + 1))
    done
    sleep 0.4                                # 结果行之后仍可能有零星输出，给个落盘余量

    out=$("$ADB" logcat -d -s leno_jni 2>/dev/null | sed 's/^.*leno_jni: //' | tr -d '\r')
    ok=$(printf '%s\n' "$out" | grep -acE '^OK ')
    ng=$(printf '%s\n' "$out" | grep -acE '^NG ')
    skip=$(printf '%s\n' "$out" | grep -acE '^SKIP ')
    note=""
    [ "$skip" -gt 0 ] && note="有 $skip 项按平台限制跳过"
    if printf '%s\n' "$out" | grep -aq '^\[错误\]'; then
        note="$note VM 报错（详见上方 logcat）"
    fi
    printf '%-12s %6s %6s  %s\n' "$p" "$ok" "$ng" "$note"

    total_ok=$((total_ok + ok)); total_ng=$((total_ng + ng))
done

printf '%-12s %6s %6s\n' 合计 "$total_ok" "$total_ng"
[ "$total_ng" -eq 0 ] && echo "=> 全部通过" || echo "=> 有失败项，用 bash android/run_probes.sh <探针名> 单独复现"
