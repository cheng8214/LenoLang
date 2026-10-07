// bench_node.js — 与 bench_leno.leno / bench_py.py 同工作量的对照基准（Node 24 / V8 JIT）
//
// 三方定位：
//   · Python 3.13  —— 同为**字节码解释器**（无 JIT）⇒ 与 LenoC 最可比的参照
//   · Node 24      —— JIT 编译到机器码 ⇒ 代表"跑分上限"，纯数值循环差距最大
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

let t = Date.now();
let sum = 0;
for (let i = 0; i < 20000000; i++) sum += i;
console.log(`int_loop_20m    = ${Date.now() - t} ms   sum=${sum}`);

t = Date.now();
let small = 0;
for (let i = 0; i < 20000000; i++) small += 1;
console.log(`int_loop_small  = ${Date.now() - t} ms   sum=${small}`);

t = Date.now();
const fv = fib(32);
console.log(`fib_32          = ${Date.now() - t} ms   fib=${fv}`);

t = Date.now();
const arr = [];
for (let i = 0; i < 1000000; i++) arr.push(i);
let s2 = 0;
for (const v of arr) s2 += v;
console.log(`array_1m_forin  = ${Date.now() - t} ms   sum=${s2}`);

t = Date.now();
let s2b = 0;
for (let i = 0; i < arr.length; i++) s2b += arr[i];
console.log(`array_1m_index  = ${Date.now() - t} ms   sum=${s2b}`);

t = Date.now();
const d = new Map();
for (let i = 0; i < 200000; i++) d.set("k" + i, i);
let s3 = 0;
for (let i = 0; i < 200000; i++) s3 += d.get("k" + i) || 0;
console.log(`dict_200k       = ${Date.now() - t} ms   sum=${s3}`);

t = Date.now();
let acc = "";
for (let i = 0; i < 20000; i++) acc += "x";
console.log(`str_concat_20k  = ${Date.now() - t} ms   len=${acc.length}`);

t = Date.now();
const parts = [];
for (let i = 0; i < 20000; i++) parts.push("x");
const joined = parts.join("");
console.log(`str_join_20k    = ${Date.now() - t} ms   len=${joined.length}`);

t = Date.now();
let hits = 0;
for (let i = 0; i < 10000; i++) if (acc.includes("xx")) hits++;
console.log(`str_find_10k    = ${Date.now() - t} ms   hits=${hits}`);

t = Date.now();
const js = '{"a":[1,2,3,{"b":"text"}],"n":123.456}';
let cnt = 0;
for (let i = 0; i < 50000; i++) {
  const o = JSON.parse(js);
  if (typeof o === "object") cnt++;
}
console.log(`json_parse_50k  = ${Date.now() - t} ms   cnt=${cnt}`);

t = Date.now();
let fs = 0;
for (let i = 0; i < 20000000; i++) fs += 0.5;
console.log(`float_loop_20m  = ${Date.now() - t} ms   sum=${fs}`);
