#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Layout/SlateRect.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

// 与引擎 SlateDebugging.h:15-17 同款守卫：裸插件编译（BuildPlugin/Fab 审核环境）里
// 本宏可能未经 SlateCore 传入，未定义时按引擎默认规则取值，避免 C4668（/sdl 下即错误）。
#ifndef WITH_SLATE_DEBUGGING
	#define WITH_SLATE_DEBUGGING !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#endif

struct FFocusEvent;
class FWidgetPath;
class FWeakWidgetPath;
class SWidget;
class UUserWidget;
class UWidget;
struct FSlateDebuggingNavigationEventArgs;
class FUiFocusSnapshotter;
class FUiFocusOverlayManager;
class FUiFocusSessionRecorder;

/** 焦点/激活事件，环形缓冲（供叠加层取最近 cause，并可后续接日志/agent 通道）。 */
struct FUiFocusEvent
{
	int64 Seq = 0;
	double TimeMs = 0.0;
	FString Type;      // FocusChanging | ActivationChanged
	int32 UserIndex = 0;
	FString Cause;     // Mouse/Navigation/SetDirectly/Cleared/OtherWidgetLostFocus/WindowActivate 或 Activated/Deactivated
	FString OldName;
	FString NewName;
	/** 所属控件名（信息栏同源判定：行边界/页面边界）。无 UMG 祖先或链失效时为空，显示回退 Name 本身。 */
	FString OldOwnerName;
	FString NewOwnerName;
	FString NewPath;

	// ---- 导航决策回放（Type=Navigation 或焦点事件 Cause=Navigation 时有效；其余为空）----
	// 枚举类字段存规范英文 token，本地化由叠加层渲染时转换。
	FString NavDirection;        // Left/Right/Up/Down/Next/Previous
	FString NavGenesis;          // Keyboard/Controller/User
	FString NavMethod;           // Explicit/CustomDelegate/CustomDelegateUnbound/NextOrPrevious/HitTestGrid/InferredExplicit
	FString NavRule;             // Escape/Explicit/Wrap/Stop/Custom/CustomBoundary
	FString NavHandlerName;      // 生效规则所在控件（引擎 Reply.Handler；推断模式=最内非 Escape 规则祖先）
	FString NavDestName;         // 目的地控件名（叶子；无目的地/Stop 为空）
	FString NavDestOwnerName;    // 目的地所属控件名（同信息栏判定）；空=无 UMG 祖先
	TArray<FString> NavRuleChain; // 源链（内→外）各控件该方向的配置 "名字|规则token"，仅列有 FNavigationMetaData 者
	TArray<FString> Stack; // 仅 uifocus.stackcapture 1 时捕获
};

/** 事件时刻的控件状态快照（仅会话记录器消费；不进 FUiFocusEvent，避免触发布局版本雷区）。 */
struct FUiFocusWidgetContext
{
	bool bValid = false;
	bool bFocusable = false;
	FString Vis;         // EVisibility::ToString()
	bool bEnabled = true;
	FSlateRect Rect = FSlateRect(0, 0, 0, 0); // 绝对坐标布局矩形
};

/** 随事件传给记录器的上下文：old/new=焦点事件两侧，dest=导航决策目的地（AttemptNavigation 捕获）。 */
struct FUiFocusEventContext
{
	FUiFocusWidgetContext Old;
	FUiFocusWidgetContext New;
	FUiFocusWidgetContext Dest;
};

/** CommonUI 桥模块注入的数据源。桥负责在自身 Shutdown 时注销。 */
class IUiFocusCommonUISource
{
public:
	virtual ~IUiFocusCommonUISource() = default;

	/**
	 * 由叠加层按 12Hz 调用（game thread）。
	 * OutEvents：自上次轮询差分出的激活/停用事件（不填 Seq，缓冲统一编号）。
	 * OutActivated：当前激活的 UCommonActivatableWidget 名字。
	 * OutInputMethod：第一个本地玩家的当前输入方式，无数据留空。
	 */
	virtual void Poll(TArray<FUiFocusEvent>& OutEvents, TArray<FString>& OutActivated, FString& OutInputMethod) = 0;
};

class FUiFocusDebuggerModule : public IModuleInterface
{
public:
	static constexpr int32 EventCapacity = 4096;

	/**
	 * Live Coding 护栏常量：FUiFocusEvent 改字段（布局变更）时 +1（会话记录器把它写进文件头，
	 * 供分析侧判断文件格式代次）。
	 */
	static constexpr uint32 EventLayoutVersion = 4;

	// inline 实现：桥接模块不产生对本模块 DLL 的符号依赖。
	static FUiFocusDebuggerModule& Get()
	{
		return FModuleManager::GetModuleChecked<FUiFocusDebuggerModule>(TEXT("UiFocusDebugger"));
	}

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void SetCommonUISource(IUiFocusCommonUISource* InSource) { CommonUISource = InSource; }
	IUiFocusCommonUISource* GetCommonUISource() const { return CommonUISource; }

	/**
	 * "页面边界"判定：给定 UserWidget 实例，返回它是否为页面级组件（如 CommonUI 的 Activatable）。
	 * 由 CommonUI 桥模块注册；未注册时所属控件判定回退为"最近候选"。
	 */
	void SetActivatablePredicate(TFunction<bool(const UUserWidget*)> InPredicate) { ActivatablePredicate = MoveTemp(InPredicate); }
	const TFunction<bool(const UUserWidget*)>& GetActivatablePredicate() const { return ActivatablePredicate; }

	/** 写入事件缓冲并分配 seq（环形缓冲视图）；InContext 同步给会话记录器（若在记录）。仅 game thread。 */
	void EnqueueEvent(FUiFocusEvent&& InEvent, FUiFocusEventContext InContext = FUiFocusEventContext());

	/** 取最近 InMaxCount 条事件（旧→新，含激活事件）。仅 game thread。 */
	void GetLatestEvents(int32 InMaxCount, TArray<FUiFocusEvent>& OutEvents);

	/** 事件侧显示名：UMG 名优先，否则 Slate 类型；无效指针返回空串。 */
	FString DescribeWidget(const TSharedPtr<SWidget>& InWidget) const;

	/** 最近一次焦点变化的 cause（叠加层信息栏用）。无事件时为空。 */
	const FString& GetLatestFocusCause() const { return LatestFocusCause; }

	/** 叠加层开关（即 uifocus.overlay cvar）。程序化入口：编辑器工具栏按钮用。跨模块调用，须带导出宏。 */
	static bool UIFOCUSDEBUGGER_API IsOverlayEnabled();
	static void UIFOCUSDEBUGGER_API SetOverlayEnabled(bool bInEnabled);

	/** 一批 SWidget* 的 UWidget 反查（一次 TObjectIterator 扫描；信息栏刷新与事件捕获共用）。 */
	static void ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets);

	/**
	 * 所属控件判定（信息栏与事件尾巴同源的唯一实现）：候选=链上 UserWidget 实例（内→外），
	 * ①列表行边界（IUserListEntry/IUserObjectListEntry）优先 ②Activatable 页面边界取边界内最外层候选
	 * ③无桥/链上无边界回退最近候选。空候选返回空串。
	 */
	FString PickOwnerName(const TArray<UUserWidget*>& InCandidatesInnerToOuter) const;

	/** 最近一次导航决策（叠加层左下面板用）；无导航数据返回 false。 */
	bool GetLatestNavDecision(FUiFocusEvent& OutNav) const;

	// ---- 会话记录（飞行记录仪）：开启期间事件逐条追加 JSONL，供 agent 事后分析 ----
	/** 开启会话（幂等）。InReason 记入文件头：overlay | cvar。堆栈捕获自动开启、结束还原。 */
	void StartSessionRecording(const FString& InReason);
	/** 结束会话并写 footer（幂等）。编辑器崩溃时无 footer，文件仍完整可用。 */
	void StopSessionRecording();
	bool IsSessionRecording() const;
	/** 叠加层 cvar / uifocus.record cvar 任一变化后调用：按优先级（record>=0 强制，否则跟随 overlay）同步会话。 */
	void SyncSessionRecording();

	/** 会话内插入标记（uifocus.mark）：分析时对准复现时刻。未在记录时无效并告警。 */
	void WriteSessionMark(const FString& InText);

	/** 把环形缓冲最近 InCount 条一次性导出成 JSONL（uifocus.dump）：崩溃/未开记录时的黑匣子取出。 */
	void DumpRingToSession(int32 InCount);

private:
	void HandleFocusChanging(const FFocusEvent& FocusEvent, const FWeakWidgetPath& OldFocusPath,
		const TSharedPtr<SWidget>& OldWidget, const FWidgetPath& NewFocusPath, const TSharedPtr<SWidget>& NewWidget);

	/** 从 FWidgetPath（根→自身，事件自带）解析所属控件名；无 UMG 祖先返回空串。 */
	FString ResolveOwnerName(const FWidgetPath& InPath) const;

	void CaptureStack(TArray<FString>& OutStack) const;

#if WITH_SLATE_DEBUGGING
	/** 引擎权威决策广播（AttemptNavigation）：填 PendingNav；焦点变化事件到达时附着，未跟随则延迟独立入队。 */
	void HandleAttemptNavigation(const FSlateDebuggingNavigationEventArgs& EventArgs);
#endif

	/** 导航决策的接收端兜底：Cause=Navigation 但无引擎广播（Explicit 回复直达路径）→ 几何推方向+沿旧链找规则。 */
	void BuildInferredNavDecision(const FWeakWidgetPath& OldFocusPath, const TSharedPtr<SWidget>& OldWidget,
		const TSharedPtr<SWidget>& NewWidget, FUiFocusEvent& InOutEvent) const;

	/** 沿路径（根→自身）内→外枚举 FNavigationMetaData 该方向规则，"名字|规则token|是否生效"，仅列有元数据者。 */
	void BuildRuleChain(const FWidgetPath& InPath, EUINavigation InDirection, const SWidget* InWinner,
		TArray<FString>& OutChain) const;

	/** 目的地的所属控件名：沿 Slate 父链构候选，行/页面边界判定与信息栏同源。 */
	FString ResolveOwnerOfWidget(const TSharedPtr<SWidget>& InWidget) const;

	/** 过期未消费的 PendingNav 以独立 Navigation 事件入队（导航未导致焦点变化的可见化）。 */
	void FlushStalePendingNav();

	// ---- 会话记录内部 ----
	/** 强制记录且叠加层关闭时，CommonUI 激活轮询的兜底 ticker（叠加层开启时其 RefreshState 已轮询）。 */
	bool TickRecorderPoll(float InDeltaTime);
	void SetRecorderPollTicker(bool bInEnable);

	/**
	 * Live Coding 护栏：FUiFocusEvent 改字段（布局变更）时把 EventLayoutVersion +1。
	 * 补丁后模块对象仍持有按旧 sizeof 分配的 EventRing——任何 TArray 操作（含析构）都按新步长
	 * 越界；唯一安全处置=整块泄漏（调试插件可接受）。容器因此用指针持有，版本不符即换新数组。
	 */
	void EnsureRingLayout();
	uint32 RingLayoutVersion = EventLayoutVersion;
	TArray<FUiFocusEvent>* EventRing = new TArray<FUiFocusEvent>();

	// 导航决策待附着槽：AttemptNavigation 先到、焦点变化事件后到（同一次导航）。
	// 一次按键是链式传递（页面→容器→行→UMG 代理层），多条焦点事件共享同一决策：
	// 附着为复制并置 Consumed，不消槽；新 AttemptNavigation 到来才重置。
	bool bHasPendingNav = false;
	bool bPendingNavConsumed = false;
	double PendingNavTimeMs = 0.0;
	FUiFocusEvent PendingNav;
	FUiFocusEvent LatestNav; // 最近一次导航决策（面板用），入队/附着时同步更新

	FDelegateHandle AttemptNavHandle;

	FDelegateHandle FocusChangingHandle;

	int64 NextSeq = 1;

	IUiFocusCommonUISource* CommonUISource = nullptr;
	TFunction<bool(const UUserWidget*)> ActivatablePredicate;
	FUiFocusSnapshotter* Snapshotter = nullptr;
	FUiFocusOverlayManager* OverlayManager = nullptr;
	FString LatestFocusCause;

	// ---- 会话记录状态（成员布局变更：本改动必须完整重启编译，禁 Live Coding）----
	FUiFocusSessionRecorder* SessionRecorder = nullptr;
	FUiFocusEventContext PendingNavCtx; // AttemptNavigation 目的地上下文，随决策附着/独立入队消费
	bool bSessionAutoStack = false;     // 会话自动开启 stackcapture
	bool bSessionPrevStack = false;     // 会话开始前的原值（结束还原）
	bool bRecorderPollActive = false;
	FTSTicker::FDelegateHandle RecorderPollHandle;
};
