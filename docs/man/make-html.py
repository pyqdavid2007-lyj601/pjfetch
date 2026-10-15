#!/usr/bin/env python3
"""make-html.py —— 把 docs/man/*.1 与 *.3 渲染为自包含 HTML 到 docs/html/。

为什么不用 groff：
    groff 的 -Thtml 设备对 CJK 支持很差——它按字节把 UTF-8 中文映射成拉丁字符实体
    （例如「多段」变成 &aring;&curren;&aelig;...），中文实际被破坏；且 groff 在
    Windows 上是可选依赖。本脚本按 man 宏子集自行渲染，UTF-8 全程无损、零依赖。

渲染规则（对应 roff 语义）：
    * 连续的文本行属于**同一个段落**，按 roff 约定合并；相邻两侧都是中日韩字符时
      不加空格（中文排版习惯），否则以空格连接。
    * 连续的 .TP 共享**一个** <dl>；每项一个 <dt> + <dd>；块级宏（.SH/.SS/.PP/.nf）
      出现时才闭合列表。
    * .B/.I 全部加粗/倾斜；.BR/.BI/.RB/.RI/.IR 按 roff 规则逐参数交替。
    * .nf … .fi 之间为预排版块（保留空白）。

支持的宏：.TH .SH .SS .TP .PP .P .B .I .BR .BI .RB .RI .IR .nf .fi .RS .RE .br
行内转义：\\fB \\fI \\fP \\fR \\\\ \\- \\(dq \\(*N \\(<= \\->

用法：python docs/man/make-html.py       （从仓库根目录执行）
"""
import glob
import html
import io
import os

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "..", "html")

CSS = """\
:root { --fg:#1f2328; --muted:#656d76; --line:#d8dee4; --bg-pre:#f6f8fa; --link:#0969da; }
* { box-sizing: border-box; }
body { max-width: 50rem; margin: 2.5rem auto 5rem; padding: 0 1.25rem;
       color: var(--fg); line-height: 1.75;
       font: 16px/1.75 -apple-system, "Segoe UI", "Microsoft YaHei", "PingFang SC", Roboto, sans-serif; }
h1 { font-size: 1.75rem; margin: 0 0 .35rem; }
h2 { font-size: 1.2rem; margin: 2.2rem 0 .8rem; padding-bottom: .3rem;
     border-bottom: 1px solid var(--line); }
h3 { font-size: 1.05rem; margin: 1.6rem 0 .6rem; }
p  { margin: .55rem 0; }
pre { margin: .7rem 0; padding: .75rem 1rem; overflow-x: auto;
      background: var(--bg-pre); border: 1px solid var(--line); border-radius: 6px;
      font: .9rem/1.6 Consolas, "Cascadia Mono", Menlo, monospace; }
code { font: .9em/1.5 Consolas, "Cascadia Mono", Menlo, monospace;
       background: var(--bg-pre); padding: .1em .35em; border-radius: 4px; }
dl { margin: .5rem 0 1.2rem; }
dt { font-weight: 600; margin-top: .9rem; }
dt + dd { margin-top: .1rem; }
dd { margin: .1rem 0 0 1.5rem; }
dd p { margin: .3rem 0; }
.meta { color: var(--muted); font-size: .85rem; margin: 0 0 1.6rem; }
"""

def CJK(ch):
    """是否中日韩字符或全角标点。"""
    return ("⺀" <= ch <= "鿿" or "　" <= ch <= "〿" or
            "＀" <= ch <= "￯")


# 行连接规则：中西文之间补空格；标点前后不留空格
NO_SPACE_BEFORE = set("，。、；：？！）】》」』”’…％·（【《「『,.;:!?)]}%")
NO_SPACE_AFTER = set("（【《「『“‘([{")


def need_space(prev_ch, next_ch):
    """连接两个源行时是否需要插入空格。"""
    if not prev_ch or not next_ch:
        return False
    if next_ch in NO_SPACE_BEFORE or prev_ch in NO_SPACE_AFTER:
        return False
    if CJK(prev_ch) and CJK(next_ch):
        return False
    return True


def inline(text):
    """一行 roff 文本 → HTML 片段（处理行内转义与强调开关）。"""
    out, i = [], 0
    bold = ital = False
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "f" and i + 2 < len(text):
                w = text[i + 2]
                if w in "BR":                      # 恢复常规字体
                    if bold: out.append("</b>"); bold = False
                    if ital: out.append("</i>"); ital = False
                elif w == "B":
                    if ital: out.append("</i>"); ital = False
                    if not bold: out.append("<b>"); bold = True
                elif w == "I":
                    if bold: out.append("</b>"); bold = False
                    if not ital: out.append("<i>"); ital = True
                i += 3
                continue
            if nxt == "\\":
                out.append("\\"); i += 2; continue
            if nxt == "-":
                out.append("-"); i += 2; continue
            if nxt == "(":
                code = text[i + 2:i + 4]
                out.append({"dq": "&quot;", "*N": "<i>N</i>", "<=": "&lt;="}.get(code, code))
                i += 4
                continue
            # 未识别的转义：原样保留反斜杠（代码示例中的 \n 等应按字面呈现）
            out.append("\\")
            i += 1
            continue
        out.append(html.escape(c))
        i += 1
    if bold: out.append("</b>")
    if ital: out.append("</i>")
    return "".join(out)


def split_args(rest):
    """.BI "带 空格" 参数 → 去掉引号、保留引号内空白。"""
    args, cur, in_q = [], [], False
    for ch in rest:
        if ch == '"':
            in_q = not in_q
            continue
        if ch.isspace() and not in_q:
            if cur:
                args.append("".join(cur)); cur = []
            continue
        cur.append(ch)
    if cur:
        args.append("".join(cur))
    return args


def emphasise(mac, rest):
    """.B/.I 全篇同体；.BR/.BI/... 逐参数交替（R = 常规体）。"""
    pattern = {"B": "B", "I": "I", "BR": "BR", "BI": "BI",
               "RB": "RB", "RI": "RI", "IR": "IR"}[mac]
    args = split_args(rest)      # roff：参数默认按空格分隔，引号仅用于含空格者
    if len(args) > 1:
        args = [a.strip() for a in args]      # 多参数时去掉自带空白，避免出现双空格
    parts = []          # [(纯文本末字符, HTML 片段)]
    for idx, a in enumerate(args):
        style = pattern[idx % len(pattern)]
        esc = inline(a)
        frag = esc if style == "R" else ("<b>%s</b>" if style == "B" else "<i>%s</i>") % esc
        parts.append((a[-1:] if a else "", frag))
    out = []
    for k, (_, frag) in enumerate(parts):
        if k and need_space(parts[k - 1][0], args[k][:1]):
            out.append(" ")
        out.append(frag)
    return "".join(out)


def render(path):
    src = io.open(path, encoding="utf-8").read().splitlines()

    tokens = []                       # (macro|None, rest)
    for ln in src:
        if ln.startswith('.\\"'):
            continue
        if ln[:1] in (".", "'"):
            parts = ln[1:].split(" ", 1)
            tokens.append((parts[0].strip(), parts[1] if len(parts) > 1 else ""))
        else:
            tokens.append((None, ln))

    title = os.path.basename(path)
    section = date = source = ""
    body = []
    in_pre = False
    in_dl = False
    in_dd = False
    para = []                         # 当前段落的 HTML 片段

    def flush_para():
        nonlocal para
        if para:
            body.append("<p>%s</p>" % "".join(para))
            para = []

    def close_dd():
        nonlocal in_dd
        if in_dd:
            flush_para()
            body.append("</dd>")
            in_dd = False

    def close_dl():
        nonlocal in_dl
        close_dd()
        if in_dl:
            body.append("</dl>")
            in_dl = False

    def add_text(html_frag):
        """把一段行内 HTML 并入当前段落（按中西文/标点规则决定是否补空格）。"""
        if para:
            prev_plain = html.unescape(para[-1][-1:]) if para[-1] else ""
            cur_plain = html.unescape(html_frag[:1])
            if need_space(prev_plain, cur_plain):
                para.append(" ")
        para.append(html_frag)

    i = 0
    expect_term = False
    while i < len(tokens):
        mac, rest = tokens[i]

        if mac == "TH":
            a = split_args(rest)
            title = a[0] if a else title
            section = a[1] if len(a) > 1 else ""
            date = a[2] if len(a) > 2 else ""
            source = a[3] if len(a) > 3 else ""
            i += 1
            continue

        if mac in ("SH", "SS"):
            close_dl()
            flush_para()
            body.append("<%s>%s</%s>" % (("h3" if mac == "SS" else "h2"),
                                         inline(rest), ("h3" if mac == "SS" else "h2")))
            i += 1
            continue

        if mac == "TP":
            flush_para()
            if not in_dl:
                body.append("<dl>")
                in_dl = True
            close_dd()
            expect_term = True
            i += 1
            continue

        if mac in ("PP", "P", "LP"):
            flush_para()
            i += 1
            continue

        if mac == "nf":
            flush_para()
            body.append("<pre>")
            in_pre = True
            i += 1
            continue
        if mac == "fi":
            if in_pre:
                body.append("</pre>")
                in_pre = False
            i += 1
            continue

        if mac in ("RS", "RE"):
            i += 1
            continue
        if mac == "br":
            if in_pre:
                body.append("")
            else:
                para.append("<br>")
            i += 1
            continue

        if mac in ("B", "I", "BR", "BI", "RB", "RI", "IR"):
            frag = emphasise(mac, rest)
            if expect_term:
                body.append("<dt>%s</dt><dd>" % frag)
                in_dd = True
                expect_term = False
            elif in_pre:
                body.append(frag)
            else:
                add_text(frag)
            i += 1
            continue

        # 普通文本行
        if in_pre:
            body.append(inline(rest))
            i += 1
            continue
        if expect_term:
            body.append("<dt>%s</dt><dd>" % inline(rest))
            in_dd = True
            expect_term = False
            i += 1
            continue
        if rest.strip():
            add_text(inline(rest))
        i += 1

    close_dl()
    flush_para()
    if in_pre:
        body.append("</pre>")

    meta = " · ".join(x for x in (source, ("section " + section if section else ""), date) if x)
    return """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title>
<style>
%s</style>
</head>
<body>
<h1>%s</h1>
<p class="meta">%s</p>
%s
</body>
</html>
""" % (html.escape(title), CSS, html.escape(title), html.escape(meta), "\n".join(body))


def main():
    outdir = os.path.normpath(OUT)
    os.makedirs(outdir, exist_ok=True)
    pages = sorted(glob.glob(os.path.join(ROOT, "*.1")) + glob.glob(os.path.join(ROOT, "*.3")))
    for p in pages:
        name = os.path.basename(p)
        io.open(os.path.join(outdir, name + ".html"), "w", encoding="utf-8").write(render(p))
        print("  %s -> docs/html/%s.html" % (name, name))
    print("完成：HTML 输出在 docs/html/")


if __name__ == "__main__":
    main()
