// 用**参考实现**解密本机真实登录态，打印「账号名 / token 长度 / dcId」（**不打印 token**）。
// 用途：与本目录的 Leno 工具做交叉验证（两者必须给出同样的账号名与 token 长度）✓
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const Em = 6, Rv = 32, rh = 64, VP = 64;
const ure = Uint8Array.from([82, 9, 106, 213, 48, 54, 165, 56, 191, 64, 163, 158, 129, 243, 215, 251, 124, 227, 57, 130, 155, 47, 255, 135, 52, 142, 67, 68, 196, 222, 233, 203, 84, 123, 148, 50, 166, 194, 35, 61, 238, 76, 149, 11, 66, 250, 195, 78, 8, 46, 161, 102, 40, 217, 36, 178, 118, 91, 162, 73, 109, 139, 209, 37]);
const dre = Uint8Array.from([31, 221, 168, 51, 136, 7, 199, 49, 177, 18, 16, 89, 39, 128, 236, 95, 96, 81, 127, 169, 25, 181, 74, 13, 45, 229, 122, 159, 147, 201, 156, 239, 160, 224, 59, 77, 174, 42, 245, 176, 200, 235, 187, 60, 131, 83, 153, 97, 23, 43, 4, 126, 186, 119, 214, 38, 225, 105, 20, 99, 85, 33, 12, 125]);
const sha512 = b => crypto.createHash('sha512').update(b).digest();

function decrypt(b64) {
    const t = Buffer.from(b64, 'base64');
    const key = t.subarray(Em, Em + Rv);
    const sha = sha512(key);
    const xor = Buffer.alloc(VP);
    for (let i = 0; i < VP; i++) xor[i] = ure[i] ^ dre[i];
    const hash = sha512(Buffer.concat([sha, xor]));
    const d = crypto.createDecipheriv('aes-128-cbc', hash.subarray(0, 16), hash.subarray(16, 32));
    const dec = Buffer.concat([d.update(t.subarray(Rv + Em)), d.final()]);
    return dec.subarray(rh).toString('utf8');
}

const brand = process.argv[2] || 'Trae CN';
const file = path.join(process.env.APPDATA, brand, 'User', 'globalStorage', 'storage.json');
if (!fs.existsSync(file)) { console.log('未找到登录态: ' + file); process.exit(2); }
const s = JSON.parse(fs.readFileSync(file, 'utf8'));
let enc = null, dc = '';
for (const k of Object.keys(s)) {
    const m = k.match(/^iCubeAuthInfo:\/\/icube-dc:(\d+)/); if (m) dc = m[1];
}
for (const k of Object.keys(s)) if (k.startsWith('iCubeAuthInfo://icube.cloudide')) enc = s[k];
const auth = JSON.parse(decrypt(enc));
console.log('[node/参考实现] 品牌=' + brand
    + ' 账号=' + ((auth.account && auth.account.username) || '')
    + ' token长度=' + ((auth.token || '').length)
    + ' 设备ID=' + dc);
