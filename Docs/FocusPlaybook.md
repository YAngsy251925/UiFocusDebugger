# FocusPlaybook — UiFocusDebugger 会话日志分析手册（agent 用）

> 本文档的读者是 agent：拿到一份会话 JSONL 后，按此手册定位焦点 BUG 的根因并给出修改方案。
> 人类读者（QA/程序员）只需知道：**打开 "UI Focus" 工具栏按钮或控制台 `uifocus.overlay 1` 即开始记录，关闭即收尾**。

## 1. 文件在哪

- 目录：`<工程>/Saved/UiFocusDebugger/sessions/`
- 最新一份：读同目录 `latest.txt`（内容为最新会话文件的绝对路径）
- 命名：`YYYY-MM-DD_HH-MM-SS.jsonl`（同秒重开加 `_2` 序号）

## 2. 文件格式（JSONL，一行一个 JSON 对象）

首行 `type:header`，中间 `type:event`，正常收尾有 `type:footer`。**没有 footer = 编辑器在会话期间崩溃**，文件仍然完整可用（每行写后 Flush）。

另有两类行：
- `type:mark`：QA 复现瞬间用控制台 `uifocus.mark "备注"` 打的标记（含 seq/t/text）——分析时直接对准标记前后的事件窗口。
- `reason:"dump"` 的会话（header.reason 字段）：`uifocus.dump` 从环形缓冲事后导出的黑匣子——**事件行没有 `ctx`**（环内未存），stack 保留；文件名与普通会话同目录。

字段说明已内嵌在 header 的 `fields` 对象里（自描述），要点：

| 字段 | 含义 |
|---|---|
| `seq` | 会话内事件序号（递增） |
| `t` | 事件时间，毫秒（开机起算；只做差值比较，不做绝对解读） |
| `kind` | `FocusChanging` / `Navigation`（决策未产生焦点变化）/ `ActivationChanged`（CommonUI 页面激活变化） |
| `cause` | Mouse / Navigation / SetDirectly / Cleared / OtherWidgetLostFocus / WindowActivate；激活事件=Activated / Deactivated |
| `old`,`new` | name=Slate 显示名（UMG 实例名优先）；owner=所属控件（列表行/Activatable 页面边界判定）；path=新焦点根到自身链 |
| `nav` | dir=方向；genesis=Keyboard/Controller/User；method=选取方式；rule=生效规则；handler=生效规则所在控件；dest/destOwner=目的地；chain=链上配置（名字\|规则\|是否生效，内→外） |
| `ctx` | 事件时刻控件状态：focusable / vis / enabled / rect（绝对坐标 [l,t,r,b]）。old=旧焦点，new=新焦点，dest=导航目的地 |
| `stack`,`pstack` | 符号化调用堆栈；pstack=其中疑似工程侧帧（启发式过滤，原文始终在 stack） |

**重要语义**：本日志是环形缓冲过滤（自环/链式折叠）**之前**的原始流。一次方向键会看到链式跳变（页面→容器→目标，同名自环=UMG 代理层的层级切换），这与叠加层显示的折叠视图不同，属正常现象。

## 3. 分析流程

1. 读 header：确认工程/地图/viewport/DPI/config（规模与配置相关的 BUG 先看这里）；记下 `startWall` 用于与 `Saved/Logs/<工程>.log` 按墙钟对齐（跨子系统关联）。
2. 用脚本做摘要（jq/python，勿整文件塞上下文）：按 `cause`/`kind` 计数；列出全部 `Navigation` 行、`Cleared` 行、`ActivationChanged` 行及其 seq。
3. 对照 QA 描述的复现时间点（或取最后一段事件），锁定可疑窗口。
4. 按 §4 的签名表匹配，形成根因假设。
5. 用 `pstack` / `owner` / `handler` 名 grep 工程源码与蓝图资产（`Content/` 下对应 WBP 的 UMG 导航配置是 `WidgetNavigation` 属性）。
6. 产出：诊断（引用 seq 行为证）+ 代码/资产位置 + 修改方案。编译验证由 agent 跑 UBT（Editor 目标），改动留人审。

## 4. 异常签名表（症状 → 假设 → 修复方向）

### 签名 1：导航请求被吞（按了没反应）
- **判据**：`kind:Navigation` 事件存在（引擎做了决策），其后 0.5s 内无对应 `FocusChanging`。
- **看什么**：`nav.dest` 为空（目的地无效）或 `ctx.dest.focusable=false`；`nav.rule=Stop`（被阻止）；chain 里全是 `Escape`（整链放行，没人接管）。
- **修复方向**：目的控件不可聚焦→查其 `Is Focusable`/Visibility；整链 Escape→在合适层级配规则（Explicit/Wrap/Custom）；Stop 误配→查配置 Stop 的控件蓝图。

### 签名 2：焦点清除与页面激活耦合
- **判据**：`cause:Cleared` 的 seq 紧邻 `kind:ActivationChanged`（Activated/Deactivated）。
- **看什么**：激活/停用的是哪个页面（name）；`pstack` 里 SetUserFocus/ClearFocus 的工程调用方。
- **修复方向**：CommonUI Activatable 的激活策略抢焦点（`DesiredFocusTarget`/`Activate` 路径）；页面切走时主动清焦的代码位置。

### 签名 3：焦点振荡
- **判据**：短时间内 A→B→A→B 交替（按 seq 与 t 差值识别）。
- **看什么**：两次反向的 cause 与 nav.rule；两个控件的 owner 是否不同页面。
- **修复方向**：两控件互相 Explicit 指向对方；或两个 Activatable 的 activate/backing 竞争。

### 签名 4：事件流静默但 QA 报"按了没反应"
- **判据**：可疑时间窗内无任何事件（既无 Navigation 也无 FocusChanging）。
- **含义**：输入没有到达 Slate——事件流只能证明"没收到"，定位不了原因。
- **排查方向**（超出本日志范围）：输入模式（Game vs UI）、视口焦点、Input Blocking、CommonUI 输入路由。此盲区高频出现时考虑给插件加输入事件钩子。

### 签名 5：推断路径方向异常
- **判据**：`nav.method=InferredExplicit`（引擎对 Explicit 回复直达路径无广播，方向由几何推断）。
- **含义**：方向/胜出规则是**推断值**，可能与真实配置意图不符——两个控件间配了 Explicit 但日志方向"不对"时，先怀疑推断而非配置。
- **修复方向**：核对链上 `Explicit` 配置是否对称（A→B 配了、B→A 没配是常见错配）。

### 补充：method=Unknown 不是数据损坏
引擎枚举 `ESlateDebuggingNavigationMethod` 首个合法值就是 `Unknown`（SlateDebugging.h:87）。Lyra 实测（2026-10-11 会话）：设置页 ListView 的条目导航全部报 `genesis=Controller + rule=Explicit + handler=ListView* + method=Unknown`——这是 CommonUI/ListView 显式条目导航的特征组合。相对地，无显式规则的屏幕（游戏主菜单）报 `method=HitTestGrid`（几何选取）。两种 regime 都正常，Unknown≠异常。

## 5. 已知局限

- 焦点只观测 Slate user 0；截图/树浏览已随 WebUI 移除（历史代码在 CHANGELOG）。
- `pstack` 分类是启发式（按符号串含工程名/插件名判断），原文以 `stack` 为准。
- `ctx` 只覆盖事件双方与导航目的地，不含全树快照（性能决策）。
- 堆栈只在 `stackCapture=true` 时有（会话默认自动开启并在结束时还原；header 的 `autoStack` 标记是否自动开启过）。
- `uifocus.record 1` 强制记录（无叠加层）时，激活事件由 0.1s 兜底轮询产生，时延略高于叠加层模式。

## 6. 反馈闭环

新发现的异常签名（症状→根因→修法）应补进 §4——签名表是本工具的核心资产，每次真实 BUG 分析后增补。
