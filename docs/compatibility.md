# SCMD 项目群兼容矩阵

三个仓库构成依赖链：**SCMD2（饭碗）← SCMD-Toolchain（地基）→ AliasOS（旗帜）**。
本表回答一个问题："这个仓库现在该用哪个版本的工具链构建，哪些组合已知有问题。"

升级流程（联动发布，见 [#9](https://github.com/SCMD-Project/SCMD-Toolchain/issues/9)）：

1. Toolchain 打 `v*` tag → Release workflow 构建并发布工件（[#7](https://github.com/SCMD-Project/SCMD-Toolchain/issues/7)）；
2. 下游仓库的 canary CI（每日定时 + 可 `workflow_dispatch` 手动触发）发现新 release，
   用新工件跑全量回归；
3. canary 绿 → 由人工把下游 `ci.yml` 的 `TOOLCHAIN_TAG`（或 `TOOLCHAIN_PIN_SHA`）升级到新版本并提交 PR；
   canary 红 → 自动开 issue 报告不兼容点，钉定版本保持不动。

| Toolchain 发布 | scmdc/scmdsim | 状态 | AliasOS 基线 | SCMD2 基线 | 说明 |
|---|---|---|---|---|---|
| **v0.13.0** | 0.13.0 | **当前稳定** | `TOOLCHAIN_TAG: v0.13.0` | 未启用（钉 0.10.0，迁移见 [SCMD2#1](https://github.com/SCMD-Project/SCMD2/issues/1)） | 哨兵别名修复（[#17](https://github.com/SCMD-Project/SCMD-Toolchain/pull/17)）；scmdsim 接受单个 .cfg 输入（[#15](https://github.com/SCMD-Project/SCMD-Toolchain/pull/15)）；examples 冒烟进 CI |
| 0.12.0 | 0.12.0 | 可用，有两处已知坑 | 仅本地 | ✗ | 见下方已知不兼容点 ① ② |
| 0.11.1 | 0.11.1 | 存档 | 0.1-dev 优化期基线（~49 MB / 11933 cfg） | ✗ | 26 组 ctest |
| 0.10.0 | 0.10.0 | SCMD2 迁移前钉定 | ✗ | `TOOLCHAIN_PIN_SHA: 27e23c179849aba2c1d6da20a583b99c49a3d716`（最后一个 0.10.0 提交） | verify_beta3 / verify_generated 基线 |

## 已知不兼容点

### ① 0.12.0 优化器折叠哨兵别名（v0.13.0 已修复）

`optimize_cfg` 的根集合漏掉 `__scmd_true` / `__scmd_false`（及 `__scmd_branch_*`），
前向折叠 + 重命名把哨兵折成 `__sN` 并删除定义。编译器内部位写入会被跟着重写所以
CPU 空转正常，但**外部 cfg 写字面量哨兵名**（如 vcsfrontgen 生成的输入页）运行时
报 `Unknown command: __scmd_true`。修复：[#17](https://github.com/SCMD-Project/SCMD-Toolchain/pull/17)。

### ② 0.12.0 state narrowing 压缩外部写入的全局（未修复，用 volatile 规避）

0.12.0 把"仅编译器内零运行时写入"的 u8 全局收窄到 1 bit。任何由**外部 cfg 在编译器
之外写值**的邮箱全局（按位组装的字节）都会落空——vcs16-calculator 的 io_lo/io_hi
页即此根因。规避：全局声明加 `volatile`。长期方案跟踪在 [#8](https://github.com/SCMD-Project/SCMD-Toolchain/issues/8) 的语义条目。

### ③ scmdc 0.10.0 → 0.11.x 语法/预算变化

0.11 引入 `const` / `compile{}` 等编译期特性并调整 alias 预算模型；SCMD2 的
13585 行生成代码按 0.10.0 行为锁定，直接换 0.12.x 编译可能触发预算或收窄差异——
这是 [SCMD2#1](https://github.com/SCMD-Project/SCMD2/issues/1) 迁移要解决的问题，
其 CI 的 canary 矩阵（钉定版 × 最新 release 各跑一遍）持续提供切换数据。

## SCB ABI 兼容性

- `pack` 产出的 `.scb` 与 scmdsim 同版本读取有回归保证；跨版本加载 `.scb` 目前**不承诺**兼容，
  下游分发包时必须连工具链版本一起钉定（AliasOS dist/cs2 工件记录其构建所用 tag）。
- 语法级 ABI（resident/export 函数名、哨兵别名、页结构）以
  [#8](https://github.com/SCMD-Project/SCMD-Toolchain/issues/8) 语言规范为准。
