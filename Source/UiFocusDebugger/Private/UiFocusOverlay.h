#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Layout/SlateRect.h"
#include "Templates/SharedPointer.h"
#include "Widgets/SLeafWidget.h" // 基类必须完整：裸编译(非 unity)下无传递包含

#include "UiFocusDebuggerModule.h" // FUiFocusEvent（RecentEvents 成员需要完整类型）

class FUiFocusOverlayManager;
class SWidget;
class UWidget;
class IConsoleVariable;

/**
 * 游戏画面内焦点叠加层（SLeafWidget，HitTestInvisible，不进命中路径、不可聚焦）。
 * 绘制：焦点控件青色虚线框（失配时变红）、hover 终端控件蓝色实线框、左上角信息栏两行小字。
 * 数据：Tick 内 12Hz 节流刷新（焦点 user 0 + 从游戏视口根遍历 hover 终端 + UMG 名反查）。
 */
class SUiFocusOverlay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SUiFocusOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual FVector2D ComputeDesiredSize(float InLayoutScaleMultiplier) const override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	/** 缓存一帧的展示状态，Tick 节流刷新，OnPaint 只读。 */
	struct FState
	{
		bool bHasFocus = false;
		FSlateRect FocusRect = FSlateRect(0, 0, 0, 0);
		FString FocusName;
		FString FocusType;
		int32 FocusUser = 0;
		FString Cause;

		bool bHasHover = false;
		FSlateRect HoverRect = FSlateRect(0, 0, 0, 0);
		FString HoverName;
		FString HoverType;
		FString HoverVis;

		bool bMismatch = false;

		/** 所属控件：Slate 链 UserWidget 实例节点 + 行/页面边界判定的宿主实例名，空表示无。 */
		FString FocusOwner;
		FString HoverOwner;

		/** CommonUI 激活栈（外→内）与输入方式；桥模块存在时每 12Hz 轮询。 */
		FString ActivationText;
		FString InputMethod;

		/** 最近事件（旧→新），右侧尾巴随叠加层常显。 */
		TArray<FUiFocusEvent> RecentEvents;

		/** 最近一次导航决策（左下明细面板）。无导航数据时 bHasNav=false。 */
		FUiFocusEvent LatestNav;
		bool bHasNav = false;
	};

	/**
	 * Live Coding 护栏（同模块 EventRing，(21) 的另一半）：FUiFocusEvent/FState 布局变更时
	 * StateLayoutVersion +1；Tick 入口检测不符即整块换新 FState、旧块泄漏不触碰
	 * （任何 TArray 操作含析构都按新步长越界）。
	 */
	static constexpr uint32 StateLayoutVersion = 2;
	void EnsureStateLayout() const;
	mutable uint32 StateVersion = StateLayoutVersion;
	mutable FState* State = new FState();

	void RefreshState();

	/** 单次 DFS：捕获焦点控件祖先链与 hover 终端祖先链（均含自身，链[0]=视口根）。 */
	static void WalkForState(SWidget* InWidget, SWidget* InFocusedWidget, TArray<SWidget*>& InOutStack,
		TArray<SWidget*>& OutFocusChain, SWidget*& OutHoverTerminal, TArray<SWidget*>& OutHoverChain);

	/** hover 终端 = 有 hover 子节点则下行，否则自身。调用方保证 InWidget IsHovered。 */
	static SWidget* FindHoverTerminal(SWidget* InWidget);

	/** 一批 SWidget* 的 UWidget 反查（委托模块同源实现：事件捕获与信息栏共用）。 */
	static void ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets);

	/**
	 * 所属控件名：Slate 祖先链上的 UserWidget 实例节点（UserWidget 本身是 UWidget，映射直取）为候选，
	 * 边界判定（行/Activatable 页面）统一走模块 PickOwnerName——与事件尾巴捕获时刻同一实现。
	 */
	static FString GetOwnerName(const TArray<SWidget*>& InChainSelfToRoot, const TMap<SWidget*, UWidget*>& InUWidgets);

	/** 单个 UWidget 的直接宿主：Outer 是本控件 WidgetTree 时取树宿主实例名，否则空。 */
	static FString GetUserWidgetName(const UWidget* InWidget);

	static void DrawDashedRect(FSlateWindowElementList& OutDrawElements, int32 InLayerId,
		const FGeometry& InAllottedGeometry, const FSlateRect& InLocalRect, const FLinearColor& InColor, float InThickness);
	static void DrawSolidRect(FSlateWindowElementList& OutDrawElements, int32 InLayerId,
		const FGeometry& InAllottedGeometry, const FSlateRect& InLocalRect, const FLinearColor& InColor, float InThickness);

	// ---- 文案本地化：跟随编辑器语言（zh* → 简中，其余 → 英文），12Hz 刷新时重查 ----
	static const TCHAR* Tr(bool bZh, const TCHAR* Zh, const TCHAR* En);
	const TCHAR* Tr2(const TCHAR* Zh, const TCHAR* En) const { return Tr(bNavTextChinese, Zh, En); }
	FString CauseDisplay(const FString& Token) const;
	FString NavRuleDisplay(const FString& Token) const;
	FString NavMethodDisplay(const FString& Token) const;
	FString NavGenesisDisplay(const FString& Token) const;
	FString NavDirectionDisplay(const FString& Token) const;
	/** "叶子(所属)"组合显示：一侧为空时只显示另一侧。 */
	static FString ComposeLeafOwner(const FString& Leaf, const FString& Owner);

	mutable bool bNavTextChinese = false;
	double LastRefreshSeconds = -1000.0;

	/** 行宽测量缓存（键="字号|文本"）：OnPaint 每帧测量而文本 12Hz 才变——RefreshState 清空，帧间命中。 */
	mutable TMap<FString, float> WidthCache;
};

/**
 * 叠加层生命周期：uifocus.overlay cvar 驱动。
 * 关闭（默认）：无控件、无 ticker、无任何每帧成本。开启：0.5s ticker 确保控件存在（PIE 结束自动重建）。
 */
class FUiFocusOverlayManager
{
public:
	void Startup();
	void Shutdown();

private:
	void OnCvarChanged(IConsoleVariable* InVariable);
	bool TickEnsure(float InDeltaTime);
	void StartTicker();
	void StopTicker();
	void DestroyOverlay();

	TWeakPtr<SUiFocusOverlay> OverlayWeak;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bTickerActive = false;
};
