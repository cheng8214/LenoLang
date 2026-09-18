// 生成"派生解密"回归用的**非机密**金标 fixture（合成 enc + 期望明文）。
//
// 为什么需要它：TraeSign（C#/JS 参考件）解密登录态的算法是
//   enc(base64) → [6B 前缀][32B key][AES-128-CBC 密文]
//   派生：sha512(key) → 与 (INV_SBOX_A ^ INV_SBOX_B) 拼成 128B → sha512 → 前 16B=aesKey、次 16B=iv
//   解密：AES-128-CBC 解出 → 丢掉前 64 字节 → 剩下的就是 JSON（含 token/account/expiredAt/region）
// 这套算法没有公开测试向量，而**真实登录态是机密**（不能进仓库）⇒ 这里用同一算法**正向加密**
// 一份合成 payload，产出一个可进仓库的 fixture；再用参考实现**反向解密**自校验一次 ✓。
//
// 用法： node gen_fixture.js   ⇒ 同目录生成 fixture_login_state.json
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

// ---- 参考件常量（trae-checkin.js 逐字照抄；左边是官方客户端派生逻辑，无需修改）----
const Em = 6, Rv = 32, rh = 64, VP = 64, q8_AES128 = 16, WP = 16;
const ure = Uint8Array.from([82, 9, 106, 213, 48, 54, 165, 56, 191, 64, 163, 158, 129, 243, 215, 251, 124, 227, 57, 130, 155, 47, 255, 135, 52, 142, 67, 68, 196, 222, 233, 203, 84, 123, 148, 50, 166, 194, 35, 61, 238, 76, 149, 11, 66, 250, 195, 78, 8, 46, 161, 102, 40, 217, 36, 178, 118, 91, 162, 73, 109, 139, 209, 37]);
const dre = Uint8Array.from([31, 221, 168, 51, 136, 7, 199, 49, 177, 18, 16, 89, 39, 128, 236, 95, 96, 81, 127, 169, 25, 181, 74, 13, 45, 229, 122, 159, 147, 201, 156, 239, 160, 224, 59, 77, 174, 42, 245, 176, 200, 235, 187, 60, 131, 83, 153, 97, 23, 43, 4, 126, 186, 119, 214, 38, 225, 105, 20, 99, 85, 33, 12, 125]);

const sha512 = buf => crypto.createHash('sha512').update(buf).digest();

function deriveAesKeyIv(key32) {
    const sha = sha512(key32);                       // 64B
    const xor = Buffer.alloc(VP);                     // 64B = ure ^ dre
    for (let i = 0; i < VP; i++) xor[i] = ure[i] ^ dre[i];
    const hash = sha512(Buffer.concat([sha, xor]));   // 128B 输入 → 64B
    return { aesKey: hash.subarray(0, q8_AES128), iv: hash.subarray(q8_AES128, q8_AES128 + WP) };
}

// 参考实现的反向解密（trae-checkin.js 的 decrypt()，逐字移植；用于自校验）
function refDecrypt(b64) {
    const t = Buffer.from(b64, 'base64');
    const key = t.subarray(Em, Em + Rv);
    const { aesKey, iv } = deriveAesKeyIv(key);
    const d = crypto.createDecipheriv('aes-128-cbc', aesKey, iv);
    const dec = Buffer.concat([d.update(t.subarray(Rv + Em)), d.final()]);
    return dec.subarray(rh).toString('utf8');
}

// ---- 1) 合成 payload（**不是**真实登录态：token 是假串）----
const payload = {
    token: 'FAKE-TOKEN-0000-1111-2222-333344445555',
    account: { username: 'demo@example.com' },
    expiredAt: 1893456000000,
    userRegion: { region: 'cn' },
    // 多字节中文也放进去，顺带验证 UTF-8 往返（真实 auth JSON 里也有中文昵称）
    note: '合成夹具·仅用于回归',
};
const json = JSON.stringify(payload);
const key32 = Buffer.from('0123456789abcdef0123456789abcdef', 'utf8');   // 固定 ⇒ fixture 可重复
const plain = Buffer.concat([Buffer.alloc(rh, 0x2a), Buffer.from(json, 'utf8')]);  // 前 64B 是占位

// ---- 2) 正向加密成 enc ----
const { aesKey, iv } = deriveAesKeyIv(key32);
const c = crypto.createCipheriv('aes-128-cbc', aesKey, iv);
const ct = Buffer.concat([c.update(plain), c.final()]);
const enc = Buffer.concat([Buffer.alloc(Em, 0x55), key32, ct]).toString('base64');

// ---- 3) 用参考实现反向解密自校验（不通过就不要产出 fixture）----
const back = refDecrypt(enc);
if (back !== json) {
    console.error('自校验失败：参考实现解回来的 JSON 与原文不一致 ✗');
    console.error(' 原文: ' + json);
    console.error(' 解回: ' + back);
    process.exit(1);
}

const out = {
    _comment: '由 tools/gen_fixture.js 生成；合成数据，非真实登录态。Leno 侧的派生解密必须与 expect 完全一致。',
    enc,
    expect: payload,
    expect_json: json,
};
fs.writeFileSync(path.join(__dirname, 'fixture_login_state.json'), JSON.stringify(out, null, 2));
// 另存两个**纯文本** fixture 供 Leno 侧直接 files.read（不依赖 JSON 模块的类型收窄 ✓）；
// 由 node 写 ⇒ **无 BOM** ✓（PowerShell 的 -Encoding UTF8 会写 BOM，会让逐字比对失败 ✗）。
fs.writeFileSync(path.join(__dirname, 'fixture_enc.txt'), enc, 'utf8');
fs.writeFileSync(path.join(__dirname, 'fixture_expect_json.txt'), json, 'utf8');
console.log('OK: fixture 已生成（自校验通过 ✓）');
console.log('  enc 长度 = ' + enc.length + ' base64 字符');
console.log('  期望 token 长度 = ' + payload.token.length);
