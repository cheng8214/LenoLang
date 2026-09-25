#!/usr/bin/env node
/**
 * tools/test_9074_retry.js — 「9074 换设备号重试」的**离线端到端**验证（node，无第三方依赖）
 *
 * 为什么需要它：9074（参与用户太多）是**服务端风控**触发的，真实账号很难按需复现 ✗。
 * 这里用一个本地 stub 服务冒充 api.trae.cn（靠 `TRAE_API_BASE` 切过去 ✓），按脚本先回 9074、
 * 再回正常结果 ⇒ 就能**确定性**验证"第一次 9074 → 换设备号 → 第二次成功"这条链路 ✓。
 * 安全性：请求全部打到 127.0.0.1，**不碰真接口** ✓（只读本机 Trae 登录态拿一个 token 用 ✓）。
 *
 * 三种场景（各隔离一条路径）：
 *   status（默认）：把**初始设备号**标成风控 ⇒ 覆盖 `fetch_status_retry` 的换号重试 ✓
 *   claim        ：状态接口一律放行，只把 **claim 第一次见到的设备号**标成风控
 *                  ⇒ 覆盖 `do_claim_retry` 的换号重试 ✓
 *   always       ：所有设备号都风控 ⇒ 覆盖"重试**打满** 5 次后放弃"（不能死循环、次数不能超 ✓）
 *
 * 用法：
 *   node tools/test_9074_retry.js              # 三种场景都跑
 *   node tools/test_9074_retry.js status       # 只跑某一种
 *   node tools/test_9074_retry.js claim
 *   node tools/test_9074_retry.js always
 *   node tools/test_9074_retry.js status "D:\CLeno\LenoC\build\lenoreg.exe"   # 指定解释器
 */
'use strict';

const http = require('http');
const path = require('path');
const { spawn } = require('child_process');

const APP_DIR = path.resolve(__dirname, '..');
const DEFAULT_LENO = path.resolve(__dirname, '../../../../build/leno.exe');
// 初始设备号（16 位数字 ⇒ 格式合法，被 stub 标成风控后**只能靠换号**过关 ✓）
const SEED_DC = '1111111111111111';

function parseArgs(argv) {
  const modes = [];
  let leno = DEFAULT_LENO;
  for (const a of argv) {
    if (a === 'status' || a === 'claim' || a === 'always') modes.push(a);
    else leno = path.resolve(a);
  }
  if (modes.length === 0) modes.push('status', 'claim', 'always');
  return { modes, leno };
}

/** 起一个 stub 服务；返回 { port, close, stats } */
function startStub(mode) {
  const stats = { seen: [], status9074: 0, claim9074: 0, checkedIn: false, claimFirstDc: null };

  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => {
      const url = String(req.url).split('?')[0];
      const dc = String(req.headers['x-device-id'] || '');
      const isStatus = url.endsWith('/checkin_credits/status');
      const isClaim = url.endsWith('/checkin_credits/claim');
      stats.seen.push({ url, dc });

      // 本场景下这个设备号该不该被风控：
      //   status 场景 ⇒ 初始号在**两个接口**上都被风控（换号是唯一出路）；
      //   claim  场景 ⇒ 状态接口一律放行，只风控 claim 第一次见到的设备号（隔离出 claim 的重试 ✓）；
      //   always 场景 ⇒ 一律风控（用来验证"重试打满"的收口 ✓）
      let flagged;
      if (mode === 'always') {
        flagged = true;
      } else if (mode === 'status') {
        flagged = dc === SEED_DC;
      } else {
        if (isClaim && stats.claimFirstDc === null) stats.claimFirstDc = dc;
        flagged = isClaim && dc === stats.claimFirstDc;
      }

      const json = (o) => res.end(JSON.stringify(o));
      if (flagged) {
        if (isStatus) stats.status9074 += 1; else if (isClaim) stats.claim9074 += 1;
        return json({ code: 9074, message: '参与用户太多' });
      }
      if (isStatus) return json({ code: 0, checked_in: stats.checkedIn, credits: 150, extra_credits: 50 });
      if (isClaim) { stats.checkedIn = true; return json({ code: 0, message: 'success' }); }
      return json({});
    });
  });

  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      resolve({
        port: server.address().port,
        stats,
        close: () => new Promise((r) => server.close(r)),
      });
    });
  });
}

/**
 * 异步跑子进程。⚠ 必须用 `spawn`（异步）而**不是** `spawnSync`：
 * spawnSync 会阻塞本进程的事件循环 ⇒ 同进程里的 stub 服务无法 accept/响应 ⇒ 子进程必然超时 ✗
 * （实测踩过：报 "Timeout was reached"，stub 一条请求都没收到）
 */
function runChild(leno, env) {
  return new Promise((resolve) => {
    const child = spawn(leno, ['--no-cache', 'trae_sign.leno'], { cwd: APP_DIR, env });
    let out = '';
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { out += d; });
    const timer = setTimeout(() => { child.kill(); }, 120000);
    child.on('close', (code) => { clearTimeout(timer); resolve({ status: code, out }); });
  });
}

function runScenario(mode, leno) {
  return startStub(mode).then((stub) => {
    const env = Object.assign({}, process.env, {
      TRAE_API_BASE: 'http://127.0.0.1:' + stub.port,
      TRAE_DEVICE_ID: SEED_DC,
    });
    return runChild(leno, env).then((r) => {
      const out = String(r.out || '');
      const checks = [];
      const check = (name, ok) => checks.push({ name, ok });

      if (out.indexOf('未找到登录态') >= 0) {
        return stub.close().then(() => ({ mode, skip: true, out, checks: [] }));
      }

      const ids = [];
      for (const s of stub.stats.seen) if (ids.indexOf(s.dc) < 0) ids.push(s.dc);
      const statusReqs = stub.stats.seen.filter((s) => s.url.endsWith('/checkin_credits/status')).length;
      const claimReqs = stub.stats.seen.filter((s) => s.url.endsWith('/checkin_credits/claim')).length;

      // 三种场景共用的检查
      check('首个设备号 = 注入的 ' + SEED_DC, ids.length > 0 && ids[0] === SEED_DC);
      check('所有设备号都是 16 位纯数字且首位非 0', ids.every((d) => /^[1-9][0-9]{15}$/.test(d)));
      check('换号后的设备号 ≠ 初始号', ids.slice(1).every((d) => d !== SEED_DC));
      check('确实收到过 9074 响应（否则等于没测到重试）',
        mode === 'status' ? stub.stats.status9074 >= 1
          : mode === 'claim' ? stub.stats.claim9074 >= 1
            : stub.stats.status9074 >= 1);

      if (mode === 'always') {
        check('打满后**失败**退出（退出码 ≠ 0）', r.status !== 0);
        check('文案点明「命中风控 9074」', out.indexOf('命中风控 9074') >= 0);
        check('文案点明「重试 5 次仍被拒」', out.indexOf('重试 5 次仍被拒') >= 0);
        check('状态接口恰好 5 次（= MAX_TRY_9074，不能死循环）', statusReqs === 5);
        check('打满后不再提交 claim', claimReqs === 0);
        check('5 次用了 5 个不同设备号', ids.length === 5);
      } else {
        check('解释器退出码 = 0', r.status === 0);
        check('输出含「签到成功」', out.indexOf('签到成功') >= 0);
        check('输出含「风控 9074：设备号已换新」', out.indexOf('风控 9074：设备号已换新') >= 0);
        check('用过 ≥2 个不同设备号（真的换了号）', ids.length >= 2);
        if (mode === 'status') {
          check('状态接口被调 ≥3 次（首次 9074 + 换号后重查 + 签到后二次确认）', statusReqs >= 3);
        } else {
          check('claim 接口恰好被调 2 次（9074 + 换号后成功）', claimReqs === 2);
        }
      }

      return stub.close().then(() => ({ mode, skip: false, out, checks, ids, statusReqs, claimReqs }));
    });
  });
}

function main() {
  const { modes, leno } = parseArgs(process.argv.slice(2));
  const fs = require('fs');
  if (!fs.existsSync(leno)) {
    console.error('找不到解释器: ' + leno);
    console.error('用法: node tools/test_9074_retry.js [status|claim] [解释器路径]');
    process.exit(2);
  }
  console.log('解释器 = ' + leno);
  console.log('应用目录 = ' + APP_DIR + '\n');

  let failed = 0;
  let skipped = 0;
  let chain = Promise.resolve();
  modes.forEach((mode) => {
    chain = chain.then(() => runScenario(mode, leno)).then((res) => {
      console.log('=== 场景 ' + res.mode + ' ===');
      if (res.skip) {
        skipped += 1;
        console.log('  SKIP：本机没有 Trae 登录态，无法跑端到端（先登录 Trae 桌面端再试）\n');
        return;
      }
      console.log('  请求序列 = ' + res.ids.join(' → '));
      console.log('  status 请求 = ' + res.statusReqs + ' 次；claim 请求 = ' + res.claimReqs + ' 次');
      for (const c of res.checks) {
        console.log('  ' + (c.ok ? 'ok   ' : 'FAIL ') + c.name);
        if (!c.ok) failed += 1;
      }
      if (res.checks.some((c) => !c.ok)) {
        console.log('  ---- 解释器输出 ----');
        console.log('  ' + res.out.trim().split('\n').join('\n  '));
      }
      console.log('');
    });
  });

  chain.then(() => {
    if (failed > 0) { console.log('test_9074_retry: ' + failed + ' 项失败'); process.exit(1); }
    if (skipped === modes.length) { console.log('test_9074_retry: 全部 SKIP（无登录态）'); process.exit(0); }
    console.log('test_9074_retry: 全部通过');
  }).catch((e) => { console.error(e); process.exit(1); });
}

main();
