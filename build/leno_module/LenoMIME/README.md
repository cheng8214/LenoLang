# LenoMIME v0.1.0

文件类型识别（**纯 Leno，零原生依赖**）：扩展名表 + 魔数嗅探 + 分类。

```leno
import "MIME" as mime

mime.byExt("D:\\a.PNG")            // "image/png"（只看扩展名，不读盘）
mime.detect("D:\\xlsx改名成.docx")  // 先读文件头 512 字节按魔数判，判不出再回退扩展名
mime.kindOf("application/zip")     // "archive"
mime.isText("application/json")    // true
```

## 为什么要有它

这张「扩展名 → MIME」表在仓库里被抄了至少三份：

| 位置 | 原来的写法 |
|---|---|
| `LenoHTTP/lib/http_static.leno` | 15 行 `if ends_with(".png") …` |
| `LenoMusic/lib/music_tag.leno` | 两处 `if ext == ".jpg" … { mime = "image/jpeg" }` |

改一处漏一处，静态服务器和音乐标签对同一个文件可能给出不同答案。而且**只看扩展名识别不了改过名的文件**（`.docx` 里装的是 xlsx，或 `.png` 里装的是 PDF）。

现在这两处都改成调 `mime.byExt`。

## API

| 函数 | 返回 | 说明 |
|---|---|---|
| `byExt(path)` | `string` | 只看扩展名（不读盘）；识别不出 ⇒ `""` |
| `byMagic(head)` | `string` | 看文件头字节（`head` 是**字节串**，512 字节够） |
| `detect(path)` | `string` | 先魔数、再扩展名；**只读前 512 字节**，读不到 ⇒ `""` |
| `detectBytes(head, pathHint)` | `string` | 已经有内容在手上时用（如 HTTP 静态服务） |
| `kindOf(mime)` | `string` | `image` / `video` / `audio` / `text` / `archive` / `font` / `executable` / `document` / `other` |
| `isText(mime)` / `isImage(mime)` | `bool` | 便捷判定 |
| `extOf(mime)` | `string` | 反查常用扩展名（不含点） |

**魔数覆盖**（前缀匹配，不做结构解析）：

- 图片：PNG / JPEG / GIF / BMP / TIFF / ICO / WebP
- 文档：PDF、OLE2 复合文档（老 doc/xls/ppt/msi）
- 归档：ZIP / 7z / RAR / gzip / bzip2 / xz / zstd / tar（`offset 257 = "ustar"`）
- 可执行：PE（`MZ`）/ ELF / WASM / Java class
- 媒体：MP3（`ID3` 或 `0xFFFB/F3/F2`）/ FLAC / OGG / WAV / MP4（`offset 4 = "ftyp"`）/ MKV-WebM / AVI
- 数据：SQLite（`"SQLite format 3\0"`）

## 口径与边界

- **识别不出就返回 `""`**，不猜、也不返回 `application/octet-stream` —— 那是 HTTP 层的默认值（`http_static` 自己补），库里不掺这一层语义
- **只看前缀**，不解析文件结构：ZIP 与 OOXML（docx/xlsx/pptx）魔数相同 ⇒ 只报 `application/zip`；要区分得读 zip 目录（那是 `LenoZip` 的事）
- `image/svg+xml` 是**两边都算**的特例：`kindOf` 判 `image`（它确实当图片用），但 `isText` 也 `true`（内容是可读 XML，能当源码预览）
- `detect` 读不到文件（不存在/无权限）⇒ `""`，不抛异常
- 扩展名比较**大小写不敏感**（内部统一 `to_lower`）

## 验证

```bat
build\leno.exe build\leno_module\LenoMIME\test\test_mime.leno
```

42 项断言：扩展名表 / 16 个魔数 / `detect` 真读盘（含"`.png` 里装 PDF 被识破"、"纯文本回退扩展名"、"文件不存在给空"）/ 分类 / 反查。
