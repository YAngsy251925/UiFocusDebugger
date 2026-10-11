#include "UiFocusDebuggerModule.h"

#include "UiFocusOverlay.h"
#include "UiFocusSessionRecorder.h"
#include "UiFocusSnapshotter.h"

#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformTime.h"
#include "Blueprint/IUserListEntry.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Debugging/SlateDebugging.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformStackWalk.h"
#include "Input/Events.h"
#include "Input/NavigationReply.h"
#include "Layout/Geometry.h"
#include "Layout/WidgetPath.h"
#include "Types/NavigationMetaData.h"
#include "Types/SlateEnums.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"

DEFINE_LOG_CATEGORY_STATIC(LogUiFocusDebugger, Log, All);

namespace
{
	TAutoConsoleVariable<bool> CVarStackCapture(TEXT("uifocus.stackcapture"), false,
		TEXT("焦点事件发生时抓调用堆栈（单次毫秒级开销，按需开启）。"));

	TAutoConsoleVariable<int> CVarRecord(TEXT("uifocus.record"), -1,
		TEXT("会话记录（飞行记录仪）：-1=跟随 uifocus.overlay（默认，叠加层开=记录）；1=强制开启（无需叠加层，CommonUI 激活事件由内置轮询补齐）；0=强制关闭。")
		TEXT("文件落 <工程>/Saved/UiFocusDebugger/sessions/*.jsonl，逐行 Flush，崩溃后仍完整；分析手册见插件 Docs/FocusPlaybook.md。"));

	const TCHAR* CauseToString(const EFocusCause Cause)
	{
		switch (Cause)
		{
		case EFocusCause::Mouse:              return TEXT("Mouse");
		case EFocusCause::Navigation:         return TEXT("Navigation");
		case EFocusCause::SetDirectly:        return TEXT("SetDirectly");
		case EFocusCause::Cleared:            return TEXT("Cleared");
		case EFocusCause::OtherWidgetLostFocus: return TEXT("OtherWidgetLostFocus");
		case EFocusCause::WindowActivate:     return TEXT("WindowActivate");
		default:                              return TEXT("Unknown");
		}
	}

	const TCHAR* NavDirectionToString(EUINavigation InDirection)
	{
		switch (InDirection)
		{
		case EUINavigation::Left:     return TEXT("Left");
		case EUINavigation::Right:    return TEXT("Right");
		case EUINavigation::Up:       return TEXT("Up");
		case EUINavigation::Down:     return TEXT("Down");
		case EUINavigation::Next:     return TEXT("Next");
		case EUINavigation::Previous: return TEXT("Previous");
		default:                      return TEXT("Invalid");
		}
	}

	const TCHAR* NavRuleToString(EUINavigationRule InRule)
	{
		switch (InRule)
		{
		case EUINavigationRule::Escape:        return TEXT("Escape");
		case EUINavigationRule::Explicit:      return TEXT("Explicit");
		case EUINavigationRule::Wrap:          return TEXT("Wrap");
		case EUINavigationRule::Stop:          return TEXT("Stop");
		case EUINavigationRule::Custom:        return TEXT("Custom");
		case EUINavigationRule::CustomBoundary: return TEXT("CustomBoundary");
		default:                               return TEXT("Invalid");
		}
	}

	const TCHAR* NavGenesisToString(ENavigationGenesis InGenesis)
	{
		switch (InGenesis)
		{
		case ENavigationGenesis::Keyboard:   return TEXT("Keyboard");
		case ENavigationGenesis::Controller: return TEXT("Controller");
		case ENavigationGenesis::User:       return TEXT("User");
		default:                             return TEXT("Unknown");
		}
	}

	const TCHAR* NavMethodToToken(ESlateDebuggingNavigationMethod InMethod)
	{
		switch (InMethod)
		{
		case ESlateDebuggingNavigationMethod::Explicit:            return TEXT("Explicit");
		case ESlateDebuggingNavigationMethod::CustomDelegateBound: return TEXT("CustomDelegate");
		case ESlateDebuggingNavigationMethod::CustomDelegateUnbound: return TEXT("CustomDelegateUnbound");
		case ESlateDebuggingNavigationMethod::NextOrPrevious:      return TEXT("NextOrPrevious");
		case ESlateDebuggingNavigationMethod::HitTestGrid:         return TEXT("HitTestGrid");
		default:                                                   return TEXT("Unknown");
		}
	}

	/** 几何推方向：旧→新中心位移的主轴。仅用于 Explicit 回复直达路径的回放标注（引擎无该路径广播）。 */
	EUINavigation InferNavDirection(const SWidget* InOldWidget, const SWidget* InNewWidget)
	{
		if (!InOldWidget || !InNewWidget)
		{
			return EUINavigation::Invalid;
		}
		// GetLayoutBoundingRect 为绝对坐标矩形
		const FSlateRect OldRect = InOldWidget->GetCachedGeometry().GetLayoutBoundingRect();
		const FSlateRect NewRect = InNewWidget->GetCachedGeometry().GetLayoutBoundingRect();
		const double Dx = NewRect.GetCenter().X - OldRect.GetCenter().X;
		const double Dy = NewRect.GetCenter().Y - OldRect.GetCenter().Y;
		if (FMath::Abs(Dx) < 0.5 && FMath::Abs(Dy) < 0.5)
		{
			return EUINavigation::Invalid;
		}
		if (FMath::Abs(Dx) > FMath::Abs(Dy))
		{
			return Dx > 0 ? EUINavigation::Right : EUINavigation::Left;
		}
		return Dy > 0 ? EUINavigation::Down : EUINavigation::Up;
	}

	/** 事件时刻控件状态快照（会话日志的"决策上下文"层）：可聚焦/可见性/可用/绝对矩形。 */
	FUiFocusWidgetContext MakeWidgetContext(const TSharedPtr<SWidget>& InWidget)
	{
		FUiFocusWidgetContext Ctx;
		if (!InWidget.IsValid())
		{
			return Ctx;
		}
		Ctx.bValid = true;
		Ctx.bFocusable = InWidget->SupportsKeyboardFocus();
		Ctx.Vis = InWidget->GetVisibility().ToString();
		Ctx.bEnabled = InWidget->IsEnabled();
		Ctx.Rect = InWidget->GetCachedGeometry().GetLayoutBoundingRect();
		return Ctx;
	}
}

void FUiFocusDebuggerModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	// CommonUI 的运行时模块在 Default 相位加载；本模块 PostDefault，晚于它。
	// 桥模块 LoadingPhase=None，只有 CommonUI 存在时才手动加载。
	if (FModuleManager::Get().IsModuleLoaded(TEXT("CommonUI")))
	{
		FModuleManager::Get().LoadModule(TEXT("UiFocusDebuggerCommonUI"));
	}

	if (FSlateApplication::IsInitialized())
	{
		FocusChangingHandle = FSlateApplication::Get().OnFocusChanging().AddRaw(
			this, &FUiFocusDebuggerModule::HandleFocusChanging);
#if WITH_SLATE_DEBUGGING
		// Development/Editor 构建的引擎权威导航决策广播（SlateApplication.cpp:6613）。
		AttemptNavHandle = FSlateDebugging::AttemptNavigationEvent.AddRaw(
			this, &FUiFocusDebuggerModule::HandleAttemptNavigation);
#endif
	}

	Snapshotter = new FUiFocusSnapshotter();
	OverlayManager = new FUiFocusOverlayManager();
	OverlayManager->Startup();

	SessionRecorder = new FUiFocusSessionRecorder();
	CVarRecord->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("UiFocusDebugger")))
		{
			FUiFocusDebuggerModule::Get().SyncSessionRecording();
		}
	}));
	SyncSessionRecording();
}

void FUiFocusDebuggerModule::ShutdownModule()
{
	if (FSlateApplication::IsInitialized() && FocusChangingHandle.IsValid())
	{
		FSlateApplication::Get().OnFocusChanging().Remove(FocusChangingHandle);
		FocusChangingHandle.Reset();
	}
#if WITH_SLATE_DEBUGGING
	if (FSlateApplication::IsInitialized() && AttemptNavHandle.IsValid())
	{
		FSlateDebugging::AttemptNavigationEvent.Remove(AttemptNavHandle);
		AttemptNavHandle.Reset();
	}
#endif

	SetRecorderPollTicker(false);
	if (SessionRecorder)
	{
		SessionRecorder->Stop(); // 写 footer；崩溃路径没有 footer，文件仍可用
		delete SessionRecorder;
		SessionRecorder = nullptr;
	}
	CVarRecord->SetOnChangedCallback(FConsoleVariableDelegate());

	if (OverlayManager)
	{
		OverlayManager->Shutdown();
		delete OverlayManager;
		OverlayManager = nullptr;
	}

	delete Snapshotter;
	Snapshotter = nullptr;
}

void FUiFocusDebuggerModule::HandleFocusChanging(const FFocusEvent& FocusEvent, const FWeakWidgetPath& OldFocusPath,
	const TSharedPtr<SWidget>& OldWidget, const FWidgetPath& NewFocusPath, const TSharedPtr<SWidget>& NewWidget)
{
	FlushStalePendingNav();

	FUiFocusEvent Ev;
	Ev.TimeMs = FPlatformTime::Seconds() * 1000.0;
	Ev.Type = TEXT("FocusChanging");
	Ev.UserIndex = (int32)FocusEvent.GetUser();
	Ev.Cause = CauseToString(FocusEvent.GetCause());
	Ev.OldName = DescribeWidget(OldWidget);
	Ev.NewName = DescribeWidget(NewWidget);

	if (FocusEvent.GetCause() == EFocusCause::Navigation)
	{
		if (bHasPendingNav)
		{
			// 同一次按键的链式传递（页面→容器→行→UMG 代理层）会产生多条焦点事件，
			// 全部附着同一份引擎决策（复制不消槽，新 AttemptNavigation 才重置）；
			// 之前"第一条消费后续走推断"会让串级第二条起被误标 InferredExplicit。
			Ev.NavDirection = PendingNav.NavDirection;
			Ev.NavGenesis = PendingNav.NavGenesis;
			Ev.NavMethod = PendingNav.NavMethod;
			Ev.NavRule = PendingNav.NavRule;
			Ev.NavHandlerName = PendingNav.NavHandlerName;
			Ev.NavDestName = PendingNav.NavDestName;
			Ev.NavDestOwnerName = PendingNav.NavDestOwnerName;
			Ev.NavRuleChain = PendingNav.NavRuleChain;
			bPendingNavConsumed = true;
		}
		else
		{
			// 无引擎广播的导航（Explicit 回复直达路径）：几何推断回放。
			BuildInferredNavDecision(OldFocusPath, OldWidget, NewWidget, Ev);
		}
	}

	if (NewWidget.IsValid())
	{
		TArray<FString> Segments;
		Segments.Reserve(NewFocusPath.Widgets.Num());
		for (int32 i = 0; i < NewFocusPath.Widgets.Num(); ++i)
		{
			Segments.Add(DescribeWidget(NewFocusPath.Widgets[i].Widget));
		}
		Ev.NewPath = FString::Join(Segments, TEXT("."));

		// 所属控件名（与信息栏同源）：FWidgetPath 自带根→自身链，无需 DFS。
		Ev.NewOwnerName = ResolveOwnerName(NewFocusPath);
	}
	// Old 侧弱引用路径：控件已销毁时 ToWidgetPath 按 Truncate 截到存活段，宿主通常仍可解析。
	if (OldWidget.IsValid())
	{
		Ev.OldOwnerName = ResolveOwnerName(OldFocusPath.ToWidgetPath());
	}

	if (CVarStackCapture.GetValueOnGameThread())
	{
		CaptureStack(Ev.Stack);
	}

	LatestFocusCause = Ev.Cause;

	// 决策上下文（会话日志用）：事件时刻两侧控件的可聚焦/可见性/可用/几何
	FUiFocusEventContext Ctx;
	Ctx.Old = MakeWidgetContext(OldWidget);
	Ctx.New = MakeWidgetContext(NewWidget);
	// 导航决策附着时并入目的地上下文——"决策说去 X 但 X 不可聚焦"是导航不动常见根因（签名1判据）
	if (bHasPendingNav && PendingNavCtx.Dest.bValid)
	{
		Ctx.Dest = PendingNavCtx.Dest;
	}

	EnqueueEvent(MoveTemp(Ev), MoveTemp(Ctx));
}

void FUiFocusDebuggerModule::CaptureStack(TArray<FString>& OutStack) const
{
	constexpr uint32 MaxDepth = 24;
	uint64 Trace[MaxDepth] = { 0 };
	const uint32 Depth = FPlatformStackWalk::CaptureStackBackTrace(Trace, MaxDepth);

	ANSICHAR Buffer[1024];
	for (uint32 i = 0; i < Depth; ++i)
	{
		// ProgramCounterToHumanReadableString 是追加写入（不清旧内容；引擎 StackWalkAndDump
		// 正是靠反复调用攒一个大串）——逐帧独立必须每帧清零，否则帧串滚雪球（实测 59→1023 字符，
		// 2026-10-11 会话文件实锤；此前堆栈从未被消费，记录仪让它现形）。
		Buffer[0] = 0;
		FPlatformStackWalk::ProgramCounterToHumanReadableString(Depth - i, Trace[i], Buffer, sizeof(Buffer));
		if (Buffer[0] != 0)
		{
			OutStack.Add(UTF8_TO_TCHAR(Buffer));
		}
	}
}

void FUiFocusDebuggerModule::EnsureRingLayout()
{
	if (RingLayoutVersion != EventLayoutVersion)
	{
		// Live Coding 补丁后，旧数组按旧 sizeof 分配：任何 TArray 操作（含析构/Empty）
		// 都按新步长越界。唯一安全处置=不触碰、整块泄漏，换新数组（调试插件可接受）。
		EventRing = new TArray<FUiFocusEvent>();
		RingLayoutVersion = EventLayoutVersion;
	}
}

void FUiFocusDebuggerModule::EnqueueEvent(FUiFocusEvent&& InEvent, FUiFocusEventContext InContext)
{
	EnsureRingLayout();
	// 会话日志在缓冲过滤（自环/链式折叠）之前写：原始流保真——链式跳变（页面→容器→目标）
	// 与自环（UMG 代理层切换）对事后分析有价值，环形缓冲语义不变（显示视图仍折叠）。
	if (SessionRecorder)
	{
		SessionRecorder->WriteEvent(InEvent, InContext);
	}
	if (InEvent.NavDirection.Len() > 0)
	{
		LatestNav = InEvent; // 面板读最近导航决策
	}

	const bool bNav = InEvent.Cause == TEXT("Navigation");
	// 自环（同名失焦/得焦 = UMG 代理层的层级切换）无信息量，不入队。
	if (bNav && InEvent.NewName.Len() > 0 && InEvent.NewName == InEvent.OldName)
	{
		return;
	}
	// 100ms 内的连续导航 = 一次按键的链式传递：折叠为最后落点（含其决策），只保留一条。
	if (bNav && EventRing->Num() > 0)
	{
		FUiFocusEvent& Top = (*EventRing)[EventRing->Num() - 1];
		if (Top.Cause == TEXT("Navigation") && InEvent.TimeMs - Top.TimeMs < 100.0)
		{
			Top = MoveTemp(InEvent);
			return;
		}
	}

	if (EventRing->Num() >= EventCapacity)
	{
		EventRing->RemoveAt(0, 1, EAllowShrinking::No);
	}
	InEvent.Seq = NextSeq++;
	EventRing->Add(MoveTemp(InEvent));
}

void FUiFocusDebuggerModule::GetLatestEvents(int32 InMaxCount, TArray<FUiFocusEvent>& OutEvents)
{
	EnsureRingLayout();
	FlushStalePendingNav();
	const int32 Count = FMath::Min(InMaxCount, EventRing->Num());
	OutEvents.Reset(Count);
	for (int32 i = EventRing->Num() - Count; i < EventRing->Num(); ++i)
	{
		OutEvents.Add((*EventRing)[i]);
	}
}

FString FUiFocusDebuggerModule::DescribeWidget(const TSharedPtr<SWidget>& InWidget) const
{
	return Snapshotter ? Snapshotter->DescribeWidget(InWidget) : FString();
}

void FUiFocusDebuggerModule::ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets)
{
	// 静态入口（叠加层以类名调用）：经单例取 Snapshotter。唯一实现收敛在 Snapshotter
	// （缓存优先+未命中定向扫描）：事件捕获与信息栏刷新共用，事件路径从"每事件两次全量扫描"
	// 降为"暖缓存后零扫描"。
	FUiFocusDebuggerModule& Module = FUiFocusDebuggerModule::Get();
	if (Module.Snapshotter)
	{
		Module.Snapshotter->ResolveUWidgets(InTargets, OutUWidgets);
	}
	else
	{
		OutUWidgets.Reset();
	}
}

FString FUiFocusDebuggerModule::PickOwnerName(const TArray<UUserWidget*>& InCandidatesInnerToOuter) const
{
	if (InCandidatesInnerToOuter.Num() == 0)
	{
		return FString();
	}

	// ① 列表行边界（优先）：候选实现了列表条目接口（IUserListEntry/IUserObjectListEntry，
	//    UMG 规定条目 widget 必需）⇒ 行内控件属于行本身——嵌在页面里的列表行（如设置页内的
	//    W_SettingsListEntry_Discrete）不再被页面边界吞掉。
	for (UUserWidget* Candidate : InCandidatesInnerToOuter)
	{
		if (Candidate->GetClass()->ImplementsInterface(UUserListEntry::StaticClass()) ||
			Candidate->GetClass()->ImplementsInterface(UUserObjectListEntry::StaticClass()))
		{
			return Candidate->GetName();
		}
	}

	// ② 页面边界：内→外第一个 Activatable（屏幕/布局等页面级组件）⇒ 答案=边界内最外层候选；
	//    最内候选即边界（焦点在页面自身控件上）或链上无边界时取最近候选。
	//    不使用 UObject Outer 链：运行期 CreateWidget 的 Outer 语义因工程而异，不可靠。
	const TFunction<bool(const UUserWidget*)>& IsBoundary = GetActivatablePredicate();
	UUserWidget* Answer = InCandidatesInnerToOuter[0];
	if (IsBoundary)
	{
		int32 BoundaryIndex = INDEX_NONE;
		for (int32 i = 0; i < InCandidatesInnerToOuter.Num(); ++i)
		{
			if (IsBoundary(InCandidatesInnerToOuter[i]))
			{
				BoundaryIndex = i;
				break;
			}
		}
		if (BoundaryIndex > 0)
		{
			Answer = InCandidatesInnerToOuter[BoundaryIndex - 1];
		}
	}
	return Answer->GetName();
}

FString FUiFocusDebuggerModule::ResolveOwnerName(const FWidgetPath& InPath) const
{
	// FWidgetPath.Widgets 即根→自身链（与叠加层 WalkForState 捕获的链同向），
	// 候选=链上的 UserWidget 实例节点（UserWidget 本身是 UWidget，映射直取），内→外排列。
	TArray<SWidget*> ChainRootToSelf;
	ChainRootToSelf.Reserve(InPath.Widgets.Num());
	for (int32 i = 0; i < InPath.Widgets.Num(); ++i)
	{
		ChainRootToSelf.Add(&InPath.Widgets[i].Widget.Get());
	}

	TMap<SWidget*, UWidget*> UWidgets;
	ResolveUWidgets(ChainRootToSelf, UWidgets);

	TArray<UUserWidget*> Candidates; // 内→外
	for (int32 i = ChainRootToSelf.Num() - 1; i >= 0; --i)
	{
		if (UWidget* const* Found = UWidgets.Find(ChainRootToSelf[i]))
		{
			if (UUserWidget* Candidate = Cast<UUserWidget>(*Found))
			{
				Candidates.Add(Candidate);
			}
		}
	}
	return PickOwnerName(Candidates);
}

#if WITH_SLATE_DEBUGGING
void FUiFocusDebuggerModule::HandleAttemptNavigation(const FSlateDebuggingNavigationEventArgs& EventArgs)
{
	const EUINavigation Direction = EventArgs.NavigationEvent.GetNavigationType();
	if (Direction == EUINavigation::Invalid)
	{
		return;
	}

	PendingNav = FUiFocusEvent();
	PendingNav.TimeMs = FPlatformTime::Seconds() * 1000.0;
	PendingNav.Type = TEXT("Navigation");
	PendingNav.NavDirection = NavDirectionToString(Direction);
	PendingNav.NavGenesis = NavGenesisToString(EventArgs.NavigationEvent.GetNavigationGenesis());
	PendingNav.NavMethod = NavMethodToToken(EventArgs.NavigationMethod);
	PendingNav.NavRule = NavRuleToString(EventArgs.NavigationReply.GetBoundaryRule());

	if (const TSharedPtr<SWidget> Handler = EventArgs.NavigationReply.GetHandler())
	{
		PendingNav.NavHandlerName = DescribeWidget(Handler);
	}
	if (EventArgs.DestinationWidget.IsValid())
	{
		PendingNav.NavDestName = DescribeWidget(EventArgs.DestinationWidget);
		PendingNav.NavDestOwnerName = ResolveOwnerOfWidget(EventArgs.DestinationWidget);
	}

	// 目的地上下文（会话日志用）："决策说去 X 但 X 不可聚焦"正是导航不动的常见根因
	PendingNavCtx = FUiFocusEventContext();
	if (EventArgs.DestinationWidget.IsValid())
	{
		PendingNavCtx.Dest = MakeWidgetContext(EventArgs.DestinationWidget);
	}

	BuildRuleChain(EventArgs.NavigationSource, Direction, EventArgs.NavigationReply.GetHandler().Get(), PendingNav.NavRuleChain);

	bHasPendingNav = true;
	bPendingNavConsumed = false;
	PendingNavTimeMs = PendingNav.TimeMs;
}
#endif

void FUiFocusDebuggerModule::BuildInferredNavDecision(const FWeakWidgetPath& OldFocusPath, const TSharedPtr<SWidget>& OldWidget,
	const TSharedPtr<SWidget>& NewWidget, FUiFocusEvent& InOutEvent) const
{
	const EUINavigation Direction = InferNavDirection(OldWidget.Get(), NewWidget.Get());
	if (Direction == EUINavigation::Invalid)
	{
		return;
	}

	// Explicit 规则（目的地随 FNavigationReply 返回）跳过 AttemptNavigation 广播（SlateApplication.cpp:3677
	// 直接 ExecuteNavigation）——此处只能推断：方向由几何得出，胜出者=旧链上最内非 Escape 规则祖先。
	FWidgetPath OldPath = OldFocusPath.ToWidgetPath();
	int32 WinnerIndex = INDEX_NONE;
	if (OldPath.IsValid())
	{
		for (int32 i = OldPath.Widgets.Num() - 1; i >= 0; --i)
		{
			const TSharedRef<SWidget>& PathWidget = OldPath.Widgets[i].Widget;
			if (const TSharedPtr<FNavigationMetaData> Meta = PathWidget->GetMetaData<FNavigationMetaData>())
			{
				const EUINavigationRule Rule = Meta->GetBoundaryRule(Direction);
				if (Rule != EUINavigationRule::Escape)
				{
					WinnerIndex = i;
					if (Rule != EUINavigationRule::Explicit)
					{
						break; // 非 Explicit 的非 Escape 规则走广播路径，不应到推断；防御性截断
					}
				}
			}
		}
	}

	InOutEvent.NavDirection = NavDirectionToString(Direction);
	InOutEvent.NavGenesis = TEXT("Unknown");
	InOutEvent.NavMethod = TEXT("InferredExplicit");
	InOutEvent.NavRule = TEXT("Explicit");
	if (OldPath.IsValid() && WinnerIndex != INDEX_NONE)
	{
		InOutEvent.NavHandlerName = DescribeWidget(OldPath.Widgets[WinnerIndex].Widget);
	}
	InOutEvent.NavDestName = InOutEvent.NewName;
	// 不读 InOutEvent.NewOwnerName——它在 HandleFocusChanging 的后续步骤才赋值（时序 bug 源）；
	// 直接对新焦点控件沿父链解析所属（与信息栏同源判定）。
	InOutEvent.NavDestOwnerName = ResolveOwnerOfWidget(NewWidget);

	if (OldPath.IsValid())
	{
		const SWidget* Winner = WinnerIndex != INDEX_NONE ? &OldPath.Widgets[WinnerIndex].Widget.Get() : nullptr;
		BuildRuleChain(OldPath, Direction, Winner, InOutEvent.NavRuleChain);
	}
}

void FUiFocusDebuggerModule::BuildRuleChain(const FWidgetPath& InPath, EUINavigation InDirection, const SWidget* InWinner,
	TArray<FString>& OutChain) const
{
	OutChain.Reset();
	// 内→外仅列配置了 FNavigationMetaData 的控件（显式参与者）；Escape 也列（配置了但放行，是常见错配来源）。
	// 行格式 "名字|规则token"：本地化由叠加层渲染时转换。
	constexpr int32 MaxChainRows = 6;
	for (int32 i = InPath.Widgets.Num() - 1; i >= 0 && OutChain.Num() < MaxChainRows; --i)
	{
		const TSharedRef<SWidget>& PathWidget = InPath.Widgets[i].Widget;
		const TSharedPtr<FNavigationMetaData> Meta = PathWidget->GetMetaData<FNavigationMetaData>();
		if (!Meta.IsValid())
		{
			continue;
		}
		const EUINavigationRule Rule = Meta->GetBoundaryRule(InDirection);
		const bool bWinner = InWinner && &PathWidget.Get() == InWinner;
		OutChain.Add(FString::Printf(TEXT("%s|%s|%d"), *DescribeWidget(PathWidget), NavRuleToString(Rule), bWinner ? 1 : 0));
	}
}

FString FUiFocusDebuggerModule::ResolveOwnerOfWidget(const TSharedPtr<SWidget>& InWidget) const
{
	// 目的地的所属控件：从目的地沿 Slate 父链构候选（与信息栏 GetOwnerName 同源判定）。
	if (!InWidget.IsValid())
	{
		return FString();
	}
	TArray<SWidget*> ChainSelfToRoot;
	ChainSelfToRoot.Reserve(32);
	{
		SWidget* Current = InWidget.Get();
		while (Current && ChainSelfToRoot.Num() < 64)
		{
			ChainSelfToRoot.Add(Current);
			Current = Current->GetParentWidget().Get();
		}
	}
	TMap<SWidget*, UWidget*> UWidgets;
	ResolveUWidgets(ChainSelfToRoot, UWidgets);
	TArray<UUserWidget*> Candidates; // 内→外
	for (int32 i = ChainSelfToRoot.Num() - 1; i >= 0; --i)
	{
		if (UWidget* const* Found = UWidgets.Find(ChainSelfToRoot[i]))
		{
			if (UUserWidget* Candidate = Cast<UUserWidget>(*Found))
			{
				Candidates.Add(Candidate);
			}
		}
	}
	return PickOwnerName(Candidates);
}

void FUiFocusDebuggerModule::FlushStalePendingNav()
{
	if (!bHasPendingNav || bPendingNavConsumed)
	{
		return;
	}
	const double NowMs = FPlatformTime::Seconds() * 1000.0;
	if (NowMs - PendingNavTimeMs < 500.0)
	{
		return;
	}
	// 决策后 0.5s 无焦点变化跟随（且未被任何事件附着）：导航未改变焦点
	// （Stop/目的地无效）——独立入队可见化。
	bPendingNavConsumed = true; // 防重复 flush；新 AttemptNavigation 重置
	EnqueueEvent(MoveTemp(PendingNav), MoveTemp(PendingNavCtx));
	PendingNav = FUiFocusEvent();
	PendingNavCtx = FUiFocusEventContext();
	bHasPendingNav = false;
}

bool FUiFocusDebuggerModule::GetLatestNavDecision(FUiFocusEvent& OutNav) const
{
	if (LatestNav.NavDirection.IsEmpty())
	{
		return false;
	}
	OutNav = LatestNav;
	return true;
}

// ---------------- 会话记录（飞行记录仪） ----------------

void FUiFocusDebuggerModule::StartSessionRecording(const FString& InReason)
{
	if (!SessionRecorder || SessionRecorder->IsActive())
	{
		return;
	}
	// 堆栈是日志分析价值最高的字段（指向工程代码位置）：会话期间自动开启，结束还原；
	// 用户中途手动关掉则尊重其值（header 的 stackCapture 记的是会话生效终值）。
	bSessionPrevStack = CVarStackCapture.GetValueOnGameThread();
	bSessionAutoStack = !bSessionPrevStack;
	if (bSessionAutoStack)
	{
		CVarStackCapture->Set(TEXT("1"), ECVF_SetByConsole);
	}

	FUiFocusSessionStartInfo Info;
	Info.Reason = InReason;
	Info.bOverlayOn = IsOverlayEnabled();
	Info.bStackCapture = CVarStackCapture.GetValueOnGameThread() != 0;
	Info.bAutoStack = bSessionAutoStack;
	Info.bActivationTracked = CommonUISource != nullptr;
	if (!SessionRecorder->Start(Info))
	{
		// 文件打开失败（磁盘满/目录只读等）：回滚自动堆栈，不留"半开"状态污染用户 cvar
		if (bSessionAutoStack)
		{
			CVarStackCapture->Set(bSessionPrevStack ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);
			bSessionAutoStack = false;
		}
	}
}

void FUiFocusDebuggerModule::StopSessionRecording()
{
	if (!SessionRecorder || !SessionRecorder->IsActive())
	{
		return;
	}
	SessionRecorder->Stop();
	if (bSessionAutoStack)
	{
		CVarStackCapture->Set(bSessionPrevStack ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);
		bSessionAutoStack = false;
	}
}

bool FUiFocusDebuggerModule::IsSessionRecording() const
{
	return SessionRecorder && SessionRecorder->IsActive();
}

void FUiFocusDebuggerModule::SyncSessionRecording()
{
	if (!SessionRecorder)
	{
		return;
	}
	const int32 Record = CVarRecord.GetValueOnGameThread();
	const bool bWant = Record >= 0 ? (Record == 1) : IsOverlayEnabled();
	if (bWant && !SessionRecorder->IsActive())
	{
		StartSessionRecording(Record >= 0 ? TEXT("cvar") : TEXT("overlay"));
	}
	else if (!bWant && SessionRecorder->IsActive())
	{
		StopSessionRecording();
	}
	// CommonUI 激活轮询兜底：仅"会话确实在记录且叠加层关"时启用（叠加层开时其 12Hz RefreshState
	// 已轮询；会话文件打开失败时轮询只入环形缓冲无意义）。
	SetRecorderPollTicker(bWant && SessionRecorder->IsActive() && !IsOverlayEnabled() && CommonUISource != nullptr);
}

bool FUiFocusDebuggerModule::TickRecorderPoll(float){
	if (IUiFocusCommonUISource* Source = CommonUISource)
	{
		TArray<FUiFocusEvent> ExternalEvents;
		TArray<FString> ActivatedNames;
		FString InputMethod;
		Source->Poll(ExternalEvents, ActivatedNames, InputMethod);
		for (FUiFocusEvent& Ev : ExternalEvents)
		{
			EnqueueEvent(MoveTemp(Ev));
		}
	}
	return true; // 持续运行，SetRecorderPollTicker(false) 移除
}

void FUiFocusDebuggerModule::SetRecorderPollTicker(bool bInEnable)
{
	if (bInEnable && !bRecorderPollActive)
	{
		RecorderPollHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("UiFocusRecorderPoll"), 0.1f,
			[this](float InDeltaTime) { return TickRecorderPoll(InDeltaTime); });
		bRecorderPollActive = true;
	}
	else if (!bInEnable && bRecorderPollActive)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RecorderPollHandle);
		RecorderPollHandle.Reset();
		bRecorderPollActive = false;
	}
}

void FUiFocusDebuggerModule::WriteSessionMark(const FString& InText)
{
	if (SessionRecorder && SessionRecorder->IsActive())
	{
		SessionRecorder->WriteMark(InText);
		UE_LOG(LogUiFocusDebugger, Log, TEXT("标记已写入：%s"), *InText);
	}
	else
	{
		UE_LOG(LogUiFocusDebugger, Warning, TEXT("uifocus.mark：当前没有记录会话（开启叠加层或 uifocus.record 1 后再标记）"));
	}
}

void FUiFocusDebuggerModule::DumpRingToSession(int32 InCount)
{
	if (!SessionRecorder)
	{
		return;
	}
	if (SessionRecorder->IsActive())
	{
		UE_LOG(LogUiFocusDebugger, Warning, TEXT("uifocus.dump：会话记录进行中，关闭会话即得完整文件，无需导出"));
		return;
	}

	// 先取后开：GetLatestEvents 会触发 FlushStalePendingNav 入队，若记录器已激活会双写
	TArray<FUiFocusEvent> Events;
	GetLatestEvents(InCount, Events);
	if (Events.Num() == 0)
	{
		UE_LOG(LogUiFocusDebugger, Warning, TEXT("uifocus.dump：环形缓冲为空，无内容可导出"));
		return;
	}

	FUiFocusSessionStartInfo Info;
	Info.Reason = TEXT("dump");
	Info.bOverlayOn = IsOverlayEnabled();
	Info.bStackCapture = CVarStackCapture.GetValueOnGameThread() != 0;
	Info.bAutoStack = false; // 事后导出，不动用户的 stackcapture
	Info.bActivationTracked = CommonUISource != nullptr;
	if (!SessionRecorder->Start(Info))
	{
		return;
	}
	for (const FUiFocusEvent& Ev : Events)
	{
		SessionRecorder->WriteEvent(Ev, FUiFocusEventContext()); // ring 未存 ctx，导出行无 ctx（stack 保留）
	}
	SessionRecorder->Stop();
	UE_LOG(LogUiFocusDebugger, Log, TEXT("uifocus.dump：已导出 %d 条 → %s"), Events.Num(), *SessionRecorder->GetFilePath());
}

// ---------------- 控制台命令 ----------------

namespace
{
	/** uifocus.mark [文本...]：会话内插标记（QA 复现瞬间打标，分析直接对准）。 */
	FAutoConsoleCommand GCmdUiFocusMark(
		TEXT("uifocus.mark"),
		TEXT("在当前记录会话中插入一条标记行（参数=备注文本，可含空格）。分析时用于对准复现时刻。"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!FModuleManager::Get().IsModuleLoaded(TEXT("UiFocusDebugger")))
			{
				return;
			}
			FUiFocusDebuggerModule::Get().WriteSessionMark(FString::Join(Args, TEXT(" ")));
		}));

	/** uifocus.dump [N]：把环形缓冲最近 N 条（默认全部，上限 4096）一次性导出为 JSONL——崩溃/未开记录时的黑匣子取出。 */
	FAutoConsoleCommand GCmdUiFocusDump(
		TEXT("uifocus.dump"),
		TEXT("把事件环形缓冲最近 N 条（默认全部）一次性导出为 JSONL 文件到 Saved/UiFocusDebugger/sessions/。用于崩溃后或忘记开启记录时的事后取证。"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!FModuleManager::Get().IsModuleLoaded(TEXT("UiFocusDebugger")))
			{
				return;
			}
			int32 Count = FUiFocusDebuggerModule::EventCapacity;
			if (Args.Num() > 0)
			{
				Count = FCString::Atoi(*Args[0]);
			}
			FUiFocusDebuggerModule::Get().DumpRingToSession(Count);
		}));
}

IMPLEMENT_MODULE(FUiFocusDebuggerModule, UiFocusDebugger)
