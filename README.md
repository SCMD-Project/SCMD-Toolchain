# SCMD

SCMD（Shortcut Command）是一门编译到 Source / Counter-Strike 2 Console CFG 的编程语言。

它最早是 2022 年写的一个 Shortcut Command 工具，后来逐渐变成了现在这套编译器和运行环境。

目前仓库里的主要工具：

* `scmdc`：编译器和项目构建工具
* `scmdsim`：CFG / SCB 模拟器
* `vcs16as` / `vcs16run` / `vcs16dump`：vCS-16/2 assembler、reference VM 与 dump 工具
* `vcs16scmd`：vCS-16/2 -> SCMD AOT backend


当前版本：`0.12.0`
SCB ABI：`1`

## 构建

需要：

* Clang / Clang++
* CMake 3.22+
* Ninja

Windows：

```powershell id="egwxtn"
.\scripts\init.ps1
.\scripts\build.ps1
```

构建后的程序在：

```text id="dhmxs3"
dist/release/scmdc.exe
dist/release/scmdsim.exe
dist/release/vcs16as.exe
dist/release/vcs16run.exe
dist/release/vcs16dump.exe
dist/release/vcs16scmd.exe
```

`out/` 只用于 CMake、Ninja 和测试产生的中间文件。

## 使用

创建一个项目：

```powershell id="xgdwtb"
scmdc init hello
cd hello
scmdc build hello.scmdproj
```

生成的 CFG 可以直接复制到 CS2 的 `cfg` 目录，然后正常执行：

```text id="rppd0x"
exec hello
```

也可以直接用 `scmdsim` 打开生成目录：

```powershell id="rt7yd9"
scmdsim build
```

```text id="o24lzm"
> exec hello
```

如果需要一个完整的二进制快照：

```powershell id="rr41z6"
scmdc pack build -o hello.scb
scmdsim hello.scb
```

## 实现

SCMD 最终不会生成 DLL，也没有需要注入游戏的运行时。

编译结果仍然是普通的 Source Console command：

```text id="4i3jfa"
alias
exec
echo
cvar
...
```

控制流、变量和算术最后都会被 lowering 成由 `alias` 组成的状态和跳转网络。

简单来说：

```text id="vrm08p"
SCMD
  ↓
parser / sema
  ↓
CFG backend
  ↓
Source Console / alias
  ↓
CS2
```

因此生成出来的程序本身就可以被真实 CS2 执行。

大型输出会被拆成多个 CFG page，避免一次向 Console command buffer 塞入过多命令。

CS2 后端现在**强制使用按需函数加载**：`main()`、全局状态、返回槽和函数入口 stub 在启动时加载；普通函数体放进 `lazy/fNNNN/` 模块，第一次调用时才 `exec`，随后入口 alias 会被真实实现覆盖。这个行为属于后端语义，不提供关闭选项；即使使用 `--no-opt`，按需加载仍然存在。

CS2 CFG 后端默认开启体积优化，会清理不可达节点、合并安全的连续跳转、去重静态 alias，并缩短编译器生成的内部符号；`wait`、`exec`、动态 alias 和分页安全仍由后端保留。需要调试或对比未优化输出时，可以使用 `scmdc ... --no-opt` 或 `scmdc build project.scmdproj --no-opt`。

## scmdsim

`scmdsim` 实现了一套和项目目标一致的 Console 环境，用于在游戏外运行和调试生成结果。

直接传 CFG root 时：

```powershell id="2kv8cb"
scmdsim build
```

不会在启动时把目录里的所有 CFG 全部编译一遍。

例如：

```text id="wabfy9"
> exec Scmd/Menu/Main
```

对应文件会在第一次执行时编译并缓存到当前进程。

修改文件后再次 `exec` 会重新读取；运行过程中新增的 CFG 也可以直接执行。

如果希望把缓存保留下来：

```powershell id="p2f917"
scmdsim build --cache
```

默认缓存目录：

```text id="f77pp7"
build/.scmdcache/
```

## SCB

SCB 是 `scmdsim` 使用的二进制格式。

目前的 SCB v1 不是另一套独立的 SCMD 后端。它从已经生成的 CFG 构建，保存 lowering 后的 Console 语义。

```text id="ksxij1"
SCMD
  ↓
CFG
  ↓
SCB
```

这样真实 CS2 和模拟器不会各自维护一套不同的执行逻辑。

SCB VM 目前使用 16 个寄存器。文件载入后会先经过 verifier，再转换到固定宽度的内部指令表示执行。

更详细的格式见：

[docs/zh-CN/bytecode.md](docs/zh-CN/bytecode.md)

## SCMD 0.11：编译期执行、固定数组与精确优化控制

0.11 的目标是让项目直接在 SCMD 源码里表达过去需要外部 source generator 才能完成的工作。编译期控制流继续使用普通 SCMD 语法，不引入第二套 `for`/range 语法。

```scmd
const N = 4;
u8 values[N] = 0;

compile
{
    for(var i = 0; i < N; i += 1)
    {
        values[i] = i + 1;
    }

    assert(values[3] == 4);
}
```

`compile {}` 在 `scmdc` 内执行，代码不会进入运行时 CFG。当前支持普通 SCMD 的 `if / while / for`、compile-local 变量、赋值、全局 fixed-array 读写和 `assert(expr)`。

固定数组当前为全局 `bool` / `u8`：

```scmd
bool used[16] = false;
u8 buffer[64] = 0;

function main()
{
    u8 i = 3;
    buffer[i] = 42;
}
```

需要保证真实 storage identity 的状态可以使用 `volatile`：

```scmd
volatile u8 host_state = 0;
```

它阻止 storage narrowing / 消除这类会让状态槽失真的优化，但仍允许不破坏可观察读写的安全优化。需要整个函数完全绕过 CFG optimizer 时使用：

```scmd
@noopt
function exact_host_bridge()
{
    host_state = 1;
    host_state = 2;
}
```

`@export` / `@resident` 也可作为函数属性；旧的 `export resident function` 语法继续兼容。

0.11 的 compile interpreter 不提供 subprocess / shell escape / 宿主文件写入；它用于确定性地构造当前程序的编译期状态，而不是把 `scmdc` 变成另一门脚本宿主。

## 示例

```scmd id="9gg3lg"
bool enabled = true;
u8 count = 10;

function main()
{
    console.clear();

    if(enabled && count >= 10)
    {
        console.print("hello from SCMD");
    }
}
```

SCMD 标识符使用 UTF-8：

```scmd id="mayyn0"
u8 数量 = 10;

function 输出()
{
    console.print("你好");
}
```

## 文档

文档入口：

[docs/zh-CN/index.md](docs/zh-CN/index.md)

实现细节、语言语法、项目格式和模拟器行为都放在 `docs/zh-CN/`。

## License

[MIT License](LICENSE)

## 0.12.0：共享加载、正确性和可诊断模拟

普通函数拥有自己的 `__scmd_loadN` load-only 入口；依赖预装载只执行这个 guard，
不复制 helper 的 alias 定义，也不运行 helper 的函数体。第一次调用时加载定义，
然后调用真实 `__scmd_fnN`；以后直接走真实入口。显式 export 和有本地存储的函数仍保留边界。

`resident` 传播到静态直接调用的函数闭包，避免最后一次 clear 后为了私有 helper 再次 exec。
**resident 不是 export**：从手写外部 CFG 调用的函数仍需要 export。
编译输出新增 `<入口文件>.loadmap.tsv`，列出函数、eager/lazy、alias 数量、页数和预加载依赖。

修复两类真实误编译：数组超出 capacity 的高位下标不再回绕到低元素；
优化后的返回目标重定向到保留名时，不再遗留已经删除的旧标签。
运行时数组越界读为 0/false，越界写不改变数组；这在普通赋值和表达式中一致。
导出名字限制为 31 字节，拒绝大小写冲突及大小写变体的原生命令名。

模拟诊断示例：

```text
scmdsim build --exec AliasOS --strict
scmdsim build --exec AliasOS --strict --echo-delay-ms 7 --exec-latency-ms 1
```

交互中 `:loads [prefix]` 列出实际成功 exec 的模块和累计次数；`:stats` 包含未知命令、
拒绝 alias 次数，以及压力参数。`--strict` 对未知命令、缺失 exec、非法 alias、
读取 CFG/输入时的过长命令返回失败；`execifexists` 找不到仍可跳过。
它不承诺识别全部游戏命令，第三方 CFG 的未建模命令也会失败。

CFG 引号内反斜杠按字面量保留，不再偷偷进行 C 风格转义；关闭 Console 不丢弃 retained log。
缓存键加入编译器版本，避免新 parser 错用旧 SCB 模块缓存。SCB ABI 仍为 1，但旧包不会自动改写
旧 parser 已经编译进去的文本；升级后应从 CFG 重新 pack。

`echo` 延迟和 `exec` 延迟是可重复压力注入，默认都是 0，**不是对真实 CS2 帧调度的精确复刻**。
模拟器不会增加任意字符串处理、pipe、con_filter 或游戏不存在的 API 来假装 AliasOS 成功。
本轮实测与限制见 [RELEASE-0.12.0.md](RELEASE-0.12.0.md)。
