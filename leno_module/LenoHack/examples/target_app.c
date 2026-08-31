/*
 * target_app.c — 内存读写实战的靶子程序
 *
 * 运行后会在控制台打印出几个变量的内存地址，
 * 然后每秒刷新显示这些变量的当前值。
 *
 * 外部程序（Leno 脚本）可以通过 ReadProcessMemory / WriteProcessMemory
 * 来读取或修改这些值，观察变化。
 *
 * 编译: gcc target_app.c -o target_app.exe
 *   或: cl target_app.c /Fe:target_app.exe
 * 运行: target_app.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* 全局变量 — 外部程序可以通过地址读写这些值 */
int g_hp     = 100;     /* 血量 */
int g_mp     = 50;      /* 蓝量 */
int g_coins  = 1000;    /* 金币 */
int g_level  = 1;       /* 等级 */
float g_speed = 5.5f;   /* 移动速度 */
char g_name[32] = "Player01"; /* 角色名 */

int main(int argc, char *argv[]) {
    /* 打印所有变量的地址，方便外部程序读写 */
    printf("========================================\n");
    printf("    Target App - Memory Read/Write Test\n");
    printf("========================================\n");
    printf("\n");
    printf("PID: %lu\n", GetCurrentProcessId());
    fflush(stdout);
    printf("\n");
    printf("--- Memory Addresses ---\n");
    printf("  g_hp     @ 0x%p  = %d\n", (void*)&g_hp,     g_hp);
    printf("  g_mp     @ 0x%p  = %d\n", (void*)&g_mp,     g_mp);
    printf("  g_coins  @ 0x%p  = %d\n", (void*)&g_coins,  g_coins);
    printf("  g_level  @ 0x%p  = %d\n", (void*)&g_level,  g_level);
    printf("  g_speed  @ 0x%p  = %.2f\n", (void*)&g_speed, g_speed);
    printf("  g_name   @ 0x%p  = %s\n", (void*)&g_name,   g_name);
    fflush(stdout);
    printf("\n");
    printf("Now run the Leno script to read/write these values.\n");
    printf("Press Ctrl+C to exit.\n");
    printf("\n");

    /* 每秒刷新显示当前值 */
    int tick = 0;
    while (1) {
        printf("[%03d] hp=%d mp=%d coins=%d level=%d speed=%.2f name=%s\n",
               tick, g_hp, g_mp, g_coins, g_level, g_speed, g_name);
        fflush(stdout);
        Sleep(1000);
        tick++;

        /* 每 5 秒自动回血 1 点（让值有变化，方便监控测试） */
        if (tick % 5 == 0 && g_hp < 100) {
            g_hp++;
        }
    }

    return 0;
}
