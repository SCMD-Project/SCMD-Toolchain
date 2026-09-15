# SCMD 0.12.0 / AliasOS 0.1.1-dev：修改与验证记录

日期：2026-09-15。基于用户上传的两个 0.11.1 源码包修改。两套项目仍独立；交付源码构建与测试不需要 Python。以下结果来自本次 Linux 本地构建和模拟器执行，不是实际 CS2 或 Windows 测试结果。

## 1. 交付内容

- `SCMD-toolchain-0.12.0-source.zip`：工具链源码、CMake 构建脚本、77 项 CTest 回归组及发布说明。
- `AliasOS-0.1.1-dev-SCMD-0.12.0-source.zip`：独立 OS 源码、Windows/Linux 构建脚本、23 项 OS 回归场景。
- `AliasOS-0.1.1-dev-CS2-deploy.zip`：本次重建的完整 CFG 部署目录，根目录为 `AliasOS.cfg` 与 `aliasos/`。
- `validation/`：实际测试日志、性能原始输出、机器可读指标、基于上传源码的补丁及本报告。

没有把 Linux 可执行文件冒充 Windows 程序。源码 ZIP 不含编译缓存或旧生成目录；OS 的 `src/generated/` 在构建时由 `vcs16scmd` 生成。

## 2. writeraw / write

### 已复现及修复

旧 `rawready` 仅设置布尔 ready 标记；没有定义正在显示的 `aos_str_N`，也能让 `end` 报 saved。修改前后的复现输出见 `raw-missing-before.log` 与 `raw-missing-after.log`。

现在分配槽时安装缺失哨兵，校验时调用当前待提交槽，哨兵回调则拒绝提交。`end` 每次重新验证，不能利用旧 ready 标志绕过；失败后仍保留编辑模式。定义正确槽后直接 `end` 即可，不再强制要求 `rawready`。新增 `rawpreview`，与兼容命令 `rawready` 共用检查/预览入口，并标注 `pending preview (not saved)`。

验证覆盖：完全未定义、定义错槽、成功预览后撤销定义、取消后再写、同一行定义并提交、UTF-8 和字面反斜杠、显式空 body、编辑期间 `cls`、已提交文件与历史内容保留，以及过长定义被拒绝。编辑中的对象不再被 `cls` 错误回收；待提交预览不会被钉进已提交历史。

### 没有伪装成已解决的边界

这个机制不是读取/解析 alias 字符串。Alias body 仍是可执行 CFG；仅应放可信的 `echoln` 文本输出或显式空 body。预览、提交校验、读取、重画可能多次执行 body，带副作用的命令不能视为安全文本。它不审计任意命令、不自动转义嵌套引号、不提供字符编辑或持久化，也不能证明任意用户 body 是有效文本。32 个对象的现有池容量保持不变。

## 3. 工具链正确性与加载

1. **数组高位漏检**：旧版长度为 4 的数组用下标 4 会误访问第 0 项，写操作也会修改错误对象。本版统一补齐 bool/u8、读取表达式/控制流选择、写入路径的范围判断：越界读返回 0/false，越界写不修改数组。长度 1、3、4、64、255、256，逐一检查所有 256 个 u8 下标，分别运行优化和不优化版本。
2. **优化器悬空返回标签**：redirect 解析到未重命名的入口/返回槽时，原实现可能保留已删除的旧标签。新数组测试发现该问题；本版写回真实最终目标，严格测试不再允许 Unknown command 悄悄通过。
3. **共享函数加载一次**：共享 helper 不再复制到多个调用者包。每个函数使用独立加载 guard，加载尾部只完成定义并关闭 guard，执行留给调用入口；保持有状态局部变量及 export 边界。私有无状态依赖通过 load-only guard 预加载。
4. **resident 传递闭包**：沿 SCMD 直接调用图保留 resident 依赖，避免绘屏关键路径再次触发模块懒加载。此分析不猜测不透明 `command.exec` 字符串中的调用。
5. **诊断输出**：生成 `<entry>.cfg.loadmap.tsv`，列出函数、驻留方式、alias/page 数量和预加载依赖。
6. **导出名检查**：目标 profile 下导出名最多 31 字节，检查大小写不敏感重名与已建模内建名称冲突。没有实现完整 CS2 cvar 注册表。

严格模拟器还发现旧 OS 的 `cfg/boot_show.cfg` 调用了未导出的 `__aos_tty_redraw_now`。已恢复导出；启动屏幕有独立最终视口断言。

## 4. 模拟器

- CFG 引号内的反斜杠按字面量处理；不再用 C 风格 `\n`、`\t` 转义冒充游戏 CFG 解析。
- 隐藏控制台时仍保留控制台日志；隐藏显示不等于删掉历史。
- `--strict` 将未知命令、无效 alias、必须存在但缺失的 exec、过长源命令等作为失败；`execifexists` 仍可合法缺省。
- `--echo-delay-ms N` 注入延迟 echo；`--exec-latency-ms N` 注入模块执行虚拟延迟。默认都是 0。输入与时间运算做溢出检查。
- `:loads [prefix]` 列出已成功执行模块及次数；`:stats` 增加去重加载数、未知命令/alias 拒绝计数与延迟参数。
- 模块缓存键加入工具链版本，避免沿用旧解析结果。SCB ABI 仍为 1；旧 SCB 必须从 CFG 重新打包才会使用修正的词法语义。

**这仍是保守兼容模型，不是完整 CS2 引擎。** 延迟不是测得的实际游戏常数；脚本逐行排空虚拟任务，不能代替实际帧并发/粘贴时序验证。旧 SCB 中已被旧编译器丢弃的命令无法从字节码恢复。缺失的 CFG 依赖也不是游戏内可事务回滚的部署错误，需要完整部署。

## 5. 本次实际测试

| 验证 | 结果 | 证据 |
|---|---:|---|
| GCC Release，最终版本 | 77/77 PASS | `ctest-final.log` |
| 独立干净目录重建后的 GCC Release | 77/77 PASS | `clean-ctest.log` |
| Clang RelWithDebInfo + ASan/UBSan，启用泄漏检测 | 77/77 PASS | `ctest-sanitize-optimized.log` |
| 独立目录 AliasOS 普通回归 | 原有 12 + 新增 11，全部通过 | `clean-aliasos.log` |
| 同一最终版本，echo=7 ms / exec=1 ms 虚拟压力 | 原有 12 + 新增 11，全部通过 | `clean-aliasos-stress.log` |
| 两版本有效读写基准 | 均读回 line1、line2；未知命令/拒绝 alias 都为 0 | `comparison-baseline.log`、`comparison-final.log` |

工具链 77 是 CTest 测试组数，不等同于仅 77 次断言；数组矩阵及优化/非优化都在这些组内。OS 的“过长输入被拒绝”是专用负例；其脚本有意使用非 strict 模式观察拒绝后的屏幕，其余正常功能回归使用 strict。Windows/PowerShell 脚本已随源代码更新，但没有在 Windows 执行。macOS、真实 CS2 未执行。

较早的一轮 Debug sanitizer 执行以及首次合并的干净构建调用受到外部工具时限中断；上述表格只报告随后完整跑完的独立验证，不将中断算作通过。

## 6. 性能：只对有效路径做比较

旧 `stats_ops.script` 使用已不存在的 `home`、`notes` 等裸 token，也没有正确创建目标文件；旧记录中的“操作完成”数字包含错误路径，不能作为有效文件读写提速依据。本版改用 `arg_*`，先创建 test 再写两行，最后 `cls` + `cat` 检查读回内容。

### 生成 CFG 的总体积

这里比较初始上传版本复现构建与最终部署构建；只累计 `.cfg` 文件的未压缩字节，不计 ZIP、JSON、TSV、源码与编译器可执行文件。

| 指标 | 上传版复现构建 | 本次最终构建 | 变化 |
|---|---:|---:|---:|
| CFG 文件数 | 11,934 | 3,889 | -67.41% |
| CFG 字节 | 28,380,143 | 9,422,304 | -66.80% |
| 十进制 MB | 28.380 | 9.422 | 同上 |

最终生成 CFG 的最长非注释命令行为 398 UTF-8 字节，最长 alias 名为 27 字节，未超过模型的 510/31 边界。不是任意未来输入都自动安全的保证。

### 同一成功工作负载

为了不让基线在启动时就调用未知命令，基线 OS **仅补上** `__aos_tty_redraw_now` 的 `export`；其余功能保持上传源码，用原 0.11.1 编译器构建。两套生成 CFG 都由**同一个最终 0.12.0 simulator**执行同一修正后的 `tests/stats_ops.script`，使用 strict，延迟均为 0。这样比较的是生成代码工作量，而不是混入两个 sim 版本的统计口径差异。基线不是逐字节未改动的上传源码，请注意这一处必要修正。

以下均为累计计数：

| 检查点 | 基线 commands | 新版 commands | 基线 execs | 新版 execs |
|---|---:|---:|---:|---:|
| 启动完成 | 31,952 | 32,534 | 554 | 566 |
| ls | 57,080 | 55,170 | 676 | 597 |
| cd /home | 132,881 | 123,493 | 1,191 | 890 |
| cat notes | 209,377 | 195,896 | 1,379 | 944 |
| touch test | 300,666 | 283,348 | 1,554 | 985 |
| writetok test（两行） | 468,937 | 450,429 | 1,676 | 1,017 |
| cls + cat test 验证 | 516,692 | 499,705 | 1,755 | 1,052 |

最终 commands 516,692 → 499,705（-3.29%），execs 1,755 → 1,052（-40.06%）。去重模块 1,736 → 1,033。

**存在代价**：冷启动 commands 31,952 → 32,534（+1.82%）；最终活跃 alias 数 38,221 → 38,887。本轮主要减少生成体积和后续加载次数，不声称所有路径或所有资源都改善。没有进行可靠的重复墙钟加载时间、CS2 帧率或 Windows 文件系统基准，不能把这些计数换算成固定游戏提速倍数。

## 7. 部署和复验

先备份自定义 CFG。结束旧游戏进程后，移除旧的 `game/csgo/cfg/AliasOS.cfg` 和 `game/csgo/cfg/aliasos/`，再将部署 ZIP 的两项根内容放入 `game/csgo/cfg/`。不要与旧生成页面合并；重启游戏也避免旧会话保留 alias 定义。当前 OS 数据在内存中，重启不是持久保存。

在允许启用 cheats 的本地测试环境按项目现有流程运行：

```text
sv_cheats 1
exec AliasOS
```

源码重建与复验方法见两个 README；`REAL_CS2_TEST_PLAN.md` 列出需要用户在真实游戏检查的输入/绘屏/提交场景。

Linux 压力测试示例（从 AliasOS 项目根目录）：

```bash
STRESS=1 ./tests/run.sh ../SCMD-toolchain-0.12.0/dist/release/scmdsim ./build
```

## 8. 原始输入校验

- `SCMD-toolchain-0.11.1-comptime-arrayload-source(1).zip`
  SHA-256: `6327b67d25032559bc4c0363cf07dfae2178e7a3c7002ab301cea5feb38f74f0`
- `AliasOS-0.1-dev-SCMD-0.11.1-optimized-no-python-source(1).zip`
  SHA-256: `07cca4e40109dcf173d1a41a1f1c4a01e4f5f03663a14509a4239944dddd8ab0`
