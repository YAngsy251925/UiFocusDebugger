#include "UiFocusOverlay.h"

#include "UiFocusDebuggerModule.h"

#include "Brushes/SlateColorBrush.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Fonts/FontMeasure.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Geometry.h"
#include "Layout/Visibility.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Styling/CoreStyle.h"
#include "Types/SlateVector2.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SViewport.h"

namespace
{
	// 视觉常量：与既定设计一致——无任何全屏元素，失配只体现为焦点框变红 + 信息栏红点。
	const FLinearColor FocusColor(0.10f, 0.85f, 0.90f, 1.0f);       // 青：正常焦点
	const FLinearColor MismatchColor(1.0f, 0.28f, 0.30f, 1.0f);     // 红：焦点丢失失配
	const FLinearColor HoverColor(0.30f, 0.62f, 1.0f, 1.0f);        // 蓝：hover 终端
	const FLinearColor InfoBgColor(0.0f, 0.0f, 0.0f, 0.75f);        // 信息栏半透明黑底
	const FLinearColor TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	const FLinearColor NavigationColor(0.72f, 0.58f, 0.95f, 1.0f);  // 导航决策行（紫）
	const FLinearColor NavChainColor(0.62f, 0.66f, 0.74f, 1.0f);    // 规则链明细（灰蓝）
	const FLinearColor NavWinColor(0.95f, 0.82f, 0.45f, 1.0f);      // 胜出规则行（金）

	const FVector2f InfoPos(8.0f, 8.0f);
	const float DotOffsetX = 9.0f;
	const FVector2f DotSize(6.0f, 6.0f);

	constexpr float RefreshIntervalSeconds = 1.0f / 12.0f;
	constexpr float DashLength = 6.0f;
	constexpr float GapLength = 4.0f;
	constexpr float LineThickness = 1.0f;
	constexpr int32 RecentEventRows = 20;

	SWidget* GetCachedRaw(const TSharedPtr<SWidget>& InWidget)
	{
		return InWidget.IsValid() ? InWidget.Get() : nullptr;
	}
}

void SUiFocusOverlay::Construct(const FArguments&)
{
	// 调试层不得污染被测系统：不进命中路径。HitTestInvisible 同时排除鼠标交互；
	// 5.8 SWidget 无 SetCanSupportFocus 设置器，bCanSupportFocus 默认关闭，键盘导航不会命中本控件。
	SetVisibility(EVisibility::HitTestInvisible);
}

FVector2D SUiFocusOverlay::ComputeDesiredSize(float) const
{
	return FVector2D::ZeroVector;
}

void SUiFocusOverlay::EnsureStateLayout() const
{
	if (StateVersion != StateLayoutVersion)
	{
		// Live Coding 补丁后旧 FState 按旧布局分配：任何访问（含重置赋值）都按新步长越界。
		// 唯一安全处置=换新块、旧块泄漏不触碰（调试控件可接受）。
		State = new FState();
		StateVersion = StateLayoutVersion;
	}
}

void SUiFocusOverlay::Tick(const FGeometry&, const double InCurrentTime, const float)
{
	EnsureStateLayout();
	if (InCurrentTime - LastRefreshSeconds < RefreshIntervalSeconds)
	{
		return;
	}
	LastRefreshSeconds = InCurrentTime;
	RefreshState();
}

void SUiFocusOverlay::WalkForState(SWidget* InWidget, SWidget* InFocusedWidget, TArray<SWidget*>& InOutStack,
	TArray<SWidget*>& OutFocusChain, SWidget*& OutHoverTerminal, TArray<SWidget*>& OutHoverChain)
{
	InOutStack.Push(InWidget);
	if (InWidget == InFocusedWidget)
	{
		OutFocusChain = InOutStack;
	}
	if (InWidget->IsHovered())
	{
		bool bChildHovered = false;
		if (FChildren* Children = InWidget->GetChildren())
		{
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				if (Children->GetChildAt(i)->IsHovered())
				{
					bChildHovered = true;
					break;
				}
			}
		}
		if (!bChildHovered)
		{
			OutHoverTerminal = InWidget;
			OutHoverChain = InOutStack;
		}
	}
	if (FChildren* Children = InWidget->GetChildren())
	{
		for (int32 i = 0; i < Children->Num(); ++i)
		{
			WalkForState(&Children->GetChildAt(i).Get(), InFocusedWidget, InOutStack, OutFocusChain, OutHoverTerminal, OutHoverChain);
		}
	}
	InOutStack.Pop();
}

SWidget* SUiFocusOverlay::FindHoverTerminal(SWidget* InWidget)
{
	if (FChildren* Children = InWidget->GetChildren())
	{
		for (int32 i = 0; i < Children->Num(); ++i)
		{
			SWidget* Child = &Children->GetChildAt(i).Get();
			if (Child->IsHovered())
			{
				return FindHoverTerminal(Child);
			}
		}
	}
	return InWidget;
}

void SUiFocusOverlay::ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets)
{
	// 同源实现收敛在模块：事件捕获（HandleFocusChanging）与信息栏刷新共用同一份反查与边界判定。
	FUiFocusDebuggerModule::ResolveUWidgets(InTargets, OutUWidgets);
}

FString SUiFocusOverlay::GetOwnerName(const TArray<SWidget*>& InChainSelfToRoot, const TMap<SWidget*, UWidget*>& InUWidgets)
{
	// 候选 = Slate 链（自身→根）上的 UserWidget 实例节点；边界判定（行/页面）统一走模块 PickOwnerName，
	// 与事件尾巴捕获时刻的判定保持同一实现。
	TArray<UUserWidget*> Candidates; // 内→外
	for (int32 i = InChainSelfToRoot.Num() - 1; i >= 0; --i)
	{
		if (UWidget* const* Found = InUWidgets.Find(InChainSelfToRoot[i]))
		{
			// UserWidget 本身是 UWidget（其 Slate 根为 SObjectWidget）：
			// 链上每个 UserWidget 实例节点经映射解析出的 UWidget 即 UUserWidget 实例本身。
			if (UUserWidget* Owner = Cast<UUserWidget>(*Found))
			{
				Candidates.Add(Owner);
			}
		}
	}
	return FUiFocusDebuggerModule::Get().PickOwnerName(Candidates);
}

FString SUiFocusOverlay::GetUserWidgetName(const UWidget* InWidget)
{
	// 焦点在视口子树外时的回退：只取本控件直属 WidgetTree 的宿主（一步，不继续上溯）。
	const UObject* Outer = InWidget ? InWidget->GetOuter() : nullptr;
	if (Outer && Outer->IsA<UWidgetTree>())
	{
		if (UUserWidget* Owner = Cast<UUserWidget>(Outer->GetOuter()))
		{
			return Owner->GetName();
		}
	}
	return FString();
}

void SUiFocusOverlay::RefreshState()
{
	EnsureStateLayout();
	*State = FState();
	WidthCache.Reset(); // 文本换代（12Hz）：测量缓存随之作废

	UGameViewportClient* GameViewport = GEngine ? GEngine->GameViewport : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = GameViewport ? GameViewport->GetGameViewportWidget() : nullptr;
	if (!ViewportWidget.IsValid())
	{
		return;
	}

	const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(0);
	const TSharedPtr<SWidget> FocusedWidget = User.IsValid() ? User->GetFocusedWidget() : nullptr;
	SWidget* FocusRaw = GetCachedRaw(FocusedWidget);

	// 单次 DFS：焦点祖先链 + hover 终端祖先链（指针在视口外时根未悬停，hover 链自然为空）
	SWidget* HoverTerminal = nullptr;
	TArray<SWidget*> Stack, FocusChain, HoverChain;
	WalkForState(ViewportWidget.Get(), FocusRaw, Stack, FocusChain, HoverTerminal, HoverChain);

	// 全部相关控件（两条链 + 视口子树外的焦点自身）的 UWidget 一次反查
	TArray<SWidget*> Targets;
	Targets.Append(FocusChain);
	if (FocusRaw && !FocusChain.Contains(FocusRaw))
	{
		Targets.Add(FocusRaw);
	}
	Targets.Append(HoverChain);
	TMap<SWidget*, UWidget*> UWidgets;
	ResolveUWidgets(Targets, UWidgets);
	UWidget** FocusUWidget = UWidgets.Find(FocusRaw);
	UWidget** HoverUWidget = UWidgets.Find(HoverTerminal);

	if (FocusRaw)
	{
		State->bHasFocus = true;
		State->FocusRect = FocusRaw->GetCachedGeometry().GetLayoutBoundingRect();
		State->FocusType = FocusRaw->GetType().ToString();
		State->FocusName = FocusUWidget ? (*FocusUWidget)->GetName() : State->FocusType;
		// 链空（焦点在视口子树外）时退回精确匹配的 UWidget 直接取所属
		State->FocusOwner = FocusChain.Num() > 0
			? GetOwnerName(FocusChain, UWidgets)
			: (FocusUWidget ? GetUserWidgetName(*FocusUWidget) : FString());
	}
	State->FocusUser = 0;
	State->Cause = FUiFocusDebuggerModule::Get().GetLatestFocusCause();

	if (HoverTerminal)
	{
		State->bHasHover = true;
		State->HoverRect = HoverTerminal->GetCachedGeometry().GetLayoutBoundingRect();
		State->HoverType = HoverTerminal->GetType().ToString();
		State->HoverVis = HoverTerminal->GetVisibility().ToString();
		State->HoverName = HoverUWidget ? (*HoverUWidget)->GetName() : State->HoverType;
		State->HoverOwner = GetOwnerName(HoverChain, UWidgets);
	}
	// 失配：无焦点但光标命中可聚焦控件
	State->bMismatch = !State->bHasFocus && HoverTerminal && HoverTerminal->SupportsKeyboardFocus();

	// CommonUI 激活栈/输入方式轮询（桥模块存在时）；差分出的激活事件入缓冲
	if (IUiFocusCommonUISource* Source = FUiFocusDebuggerModule::Get().GetCommonUISource())
	{
		TArray<FUiFocusEvent> ExternalEvents;
		TArray<FString> ActivatedNames;
		FString InputMethod;
		Source->Poll(ExternalEvents, ActivatedNames, InputMethod);
		for (FUiFocusEvent& Ev : ExternalEvents)
		{
			FUiFocusDebuggerModule::Get().EnqueueEvent(MoveTemp(Ev));
		}
		State->ActivationText = FString::Join(ActivatedNames, TEXT(" < "));
		State->InputMethod = MoveTemp(InputMethod);
	}

	// 事件尾巴数据（旧→新，最新在底部）
	// 事件尾巴数据（旧→新，最新在底部）
	FUiFocusDebuggerModule::Get().GetLatestEvents(RecentEventRows, State->RecentEvents);

	// 最近导航决策（左下明细面板）+ 文案语言跟随编辑器（zh* → 简中，其余 → 英文）
	State->bHasNav = FUiFocusDebuggerModule::Get().GetLatestNavDecision(State->LatestNav);
	bNavTextChinese = FInternationalization::Get().GetCurrentCulture()->GetName().StartsWith(TEXT("zh"));
}

const TCHAR* SUiFocusOverlay::Tr(bool bZh, const TCHAR* Zh, const TCHAR* En)
{
	return bZh ? Zh : En;
}

FString SUiFocusOverlay::CauseDisplay(const FString& Token) const
{
	if (Token == TEXT("Mouse"))               return Tr2(TEXT("鼠标"), TEXT("Mouse"));
	if (Token == TEXT("Navigation"))          return Tr2(TEXT("导航"), TEXT("Navigation"));
	if (Token == TEXT("SetDirectly"))         return Tr2(TEXT("直接设置"), TEXT("SetDirectly"));
	if (Token == TEXT("Cleared"))             return Tr2(TEXT("清除"), TEXT("Cleared"));
	if (Token == TEXT("OtherWidgetLostFocus")) return Tr2(TEXT("他控件失焦"), TEXT("OtherLostFocus"));
	if (Token == TEXT("WindowActivate"))      return Tr2(TEXT("窗口激活"), TEXT("WindowActivate"));
	if (Token == TEXT("Activated"))           return Tr2(TEXT("激活"), TEXT("Activated"));
	if (Token == TEXT("Deactivated"))         return Tr2(TEXT("停用"), TEXT("Deactivated"));
	return Token;
}

FString SUiFocusOverlay::NavRuleDisplay(const FString& Token) const
{
	// 规则名后带直白解释（Escape=冒泡给上级等）；英文侧保留引擎词。
	if (Token == TEXT("Escape"))          return Tr2(TEXT("Escape(冒泡给上级)"), TEXT("Escape(bubble up)"));
	if (Token == TEXT("Explicit"))        return Tr2(TEXT("Explicit(指定控件)"), TEXT("Explicit(target widget)"));
	if (Token == TEXT("Wrap"))            return Tr2(TEXT("Wrap(循环)"), TEXT("Wrap(wrap around)"));
	if (Token == TEXT("Stop"))            return Tr2(TEXT("Stop(阻止)"), TEXT("Stop(blocked)"));
	if (Token == TEXT("Custom"))          return Tr2(TEXT("Custom(自定义)"), TEXT("Custom(custom)"));
	if (Token == TEXT("CustomBoundary"))  return Tr2(TEXT("CustomBoundary(边界自定义)"), TEXT("CustomBoundary(custom at boundary)"));
	return Token;
}

FString SUiFocusOverlay::NavMethodDisplay(const FString& Token) const
{
	// 方法=引擎如何选出路：有规则命中走规则，否则按位置几何推算。
	if (Token == TEXT("Explicit"))              return Tr2(TEXT("指定规则"), TEXT("explicit rule"));
	if (Token == TEXT("CustomDelegate"))        return Tr2(TEXT("自定义委托"), TEXT("custom delegate"));
	if (Token == TEXT("CustomDelegateUnbound")) return Tr2(TEXT("自定义委托(未绑定)"), TEXT("custom delegate (unbound)"));
	if (Token == TEXT("NextOrPrevious"))        return TEXT("Next/Previous");
	if (Token == TEXT("HitTestGrid"))           return Tr2(TEXT("几何推算(无规则命中)"), TEXT("geometric (no rule hit)"));
	if (Token == TEXT("InferredExplicit"))      return Tr2(TEXT("指定规则(推断)"), TEXT("explicit rule (inferred)"));
	return Token;
}

FString SUiFocusOverlay::NavGenesisDisplay(const FString& Token) const
{
	if (Token == TEXT("Keyboard"))   return Tr2(TEXT("键盘"), TEXT("keyboard"));
	if (Token == TEXT("Controller")) return Tr2(TEXT("手柄"), TEXT("controller"));
	if (Token == TEXT("User"))       return Tr2(TEXT("代码触发"), TEXT("code"));
	return Token;
}

FString SUiFocusOverlay::NavDirectionDisplay(const FString& Token) const
{
	if (Token == TEXT("Left"))     return Tr2(TEXT("左"), TEXT("Left"));
	if (Token == TEXT("Right"))    return Tr2(TEXT("右"), TEXT("Right"));
	if (Token == TEXT("Up"))       return Tr2(TEXT("上"), TEXT("Up"));
	if (Token == TEXT("Down"))     return Tr2(TEXT("下"), TEXT("Down"));
	if (Token == TEXT("Next"))     return Tr2(TEXT("下一个"), TEXT("Next"));
	if (Token == TEXT("Previous")) return Tr2(TEXT("上一个"), TEXT("Previous"));
	return Token;
}

FString SUiFocusOverlay::ComposeLeafOwner(const FString& Leaf, const FString& Owner)
{
	if (Leaf.IsEmpty())
	{
		return Owner;
	}
	if (Owner.IsEmpty())
	{
		return Leaf;
	}
	return Leaf + TEXT("(") + Owner + TEXT(")");
}

void SUiFocusOverlay::DrawDashedRect(FSlateWindowElementList& OutDrawElements, int32 InLayerId,
	const FGeometry& InAllottedGeometry, const FSlateRect& InLocalRect, const FLinearColor& InColor, float InThickness)
{
	auto DrawSegment = [&OutDrawElements, &InAllottedGeometry, InLayerId, InColor, InThickness](FVector2f A, FVector2f B)
	{
		TArray<FVector2f> Points;
		Points.Add(A);
		Points.Add(B);
		FSlateDrawElement::MakeLines(OutDrawElements, InLayerId, InAllottedGeometry.ToPaintGeometry(),
			MoveTemp(Points), ESlateDrawEffect::None, InColor, /*bAntialias*/false, InThickness);
	};

	// 四边逐段画虚线（dash/gap 交替）
	const FVector2f Edges[4][2] =
	{
		{ FVector2f(InLocalRect.Left,  InLocalRect.Top),    FVector2f(InLocalRect.Right, InLocalRect.Top) },
		{ FVector2f(InLocalRect.Right, InLocalRect.Top),    FVector2f(InLocalRect.Right, InLocalRect.Bottom) },
		{ FVector2f(InLocalRect.Right, InLocalRect.Bottom), FVector2f(InLocalRect.Left,  InLocalRect.Bottom) },
		{ FVector2f(InLocalRect.Left,  InLocalRect.Bottom), FVector2f(InLocalRect.Left,  InLocalRect.Top) }
	};

	for (int32 EdgeIndex = 0; EdgeIndex < 4; ++EdgeIndex)
	{
		const FVector2f Start = Edges[EdgeIndex][0];
		const FVector2f End = Edges[EdgeIndex][1];
		const FVector2f Delta = End - Start;
		const float EdgeLength = Delta.Size();
		if (EdgeLength < 1.0f)
		{
			continue;
		}
		const FVector2f Dir = Delta / EdgeLength;

		float Walked = 0.0f;
		while (Walked < EdgeLength)
		{
			const float DashEnd = FMath::Min(Walked + DashLength, EdgeLength);
			DrawSegment(Start + Dir * Walked, Start + Dir * DashEnd);
			Walked = DashEnd + GapLength;
		}
	}
}

void SUiFocusOverlay::DrawSolidRect(FSlateWindowElementList& OutDrawElements, int32 InLayerId,
	const FGeometry& InAllottedGeometry, const FSlateRect& InLocalRect, const FLinearColor& InColor, float InThickness)
{
	TArray<FVector2f> Points;
	Points.Add(FVector2f(InLocalRect.Left,  InLocalRect.Top));
	Points.Add(FVector2f(InLocalRect.Right, InLocalRect.Top));
	Points.Add(FVector2f(InLocalRect.Right, InLocalRect.Bottom));
	Points.Add(FVector2f(InLocalRect.Left,  InLocalRect.Bottom));
	Points.Add(FVector2f(InLocalRect.Left,  InLocalRect.Top));
	FSlateDrawElement::MakeLines(OutDrawElements, InLayerId, InAllottedGeometry.ToPaintGeometry(),
		MoveTemp(Points), ESlateDrawEffect::None, InColor, /*bAntialias*/false, InThickness);
}

int32 SUiFocusOverlay::OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry, const FSlateRect&,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle&, bool) const
{
	// OnPaint 先于首帧 Tick 到达时 State 也必须是本代码布局的块。
	EnsureStateLayout();

	// 绝对坐标 → 本控件局部坐标。AbsoluteToLocal 返回类型随配置为 FVector2f 或 FDeprecateSlateVector2D，
	// CastToVector2f 对两者都有重载，保证两种配置可编译。
	const auto ToLocalRect = [&AllottedGeometry](const FSlateRect& InAbsolute)
	{
		const FVector2f TopLeft = UE::Slate::CastToVector2f(AllottedGeometry.AbsoluteToLocal(FVector2f(InAbsolute.Left, InAbsolute.Top)));
		const FVector2f BottomRight = UE::Slate::CastToVector2f(AllottedGeometry.AbsoluteToLocal(FVector2f(InAbsolute.Right, InAbsolute.Bottom)));
		return FSlateRect(TopLeft.X, TopLeft.Y, BottomRight.X, BottomRight.Y);
	};

	// 面板宽自适应：逐行实测像素宽取最大（宁宽勿截），上限=视口宽 70%；超上限的行按比例估字符数保尾截断。
	// 测量走 WidthCache（RefreshState 清空、帧间命中）：OnPaint 每帧调用，文本 12Hz 才变。
	const TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const auto MeasureW = [this, &FontMeasure](const FString& Text, const FSlateFontInfo& InFont) -> float
	{
		FString CacheKey = FString::Printf(TEXT("%d|%s"), static_cast<int32>(InFont.Size), *Text);
		if (const float* Cached = WidthCache.Find(CacheKey))
		{
			return *Cached;
		}
		const float W = FontMeasure->Measure(Text, InFont).X;
		WidthCache.Add(MoveTemp(CacheKey), W);
		return W;
	};
	const auto TruncTailN = [](const FString& In, int32 MaxLen) -> FString
	{
		return In.Len() <= MaxLen ? In : TEXT("…") + In.Right(FMath::Max(MaxLen, 1));
	};
	const FVector2f LocalSize(UE::Slate::CastToVector2f(AllottedGeometry.GetLocalSize()));
	const float WidthCap = FMath::Max(LocalSize.X * 0.7f, 320.0f);

	int32 CurrentLayer = LayerId;

	// 1) hover 终端：蓝色实线框
	if (State->bHasHover)
	{
		DrawSolidRect(OutDrawElements, CurrentLayer++, AllottedGeometry, ToLocalRect(State->HoverRect), HoverColor, LineThickness);
	}

	// 2) 焦点：青色虚线框（失配的定义就是焦点不存在，故失配信号只由信息栏红点承担）+ 实例名标签
	if (State->bHasFocus)
	{
		const FSlateRect FocusLocal = ToLocalRect(State->FocusRect);
		DrawDashedRect(OutDrawElements, CurrentLayer++, AllottedGeometry, FocusLocal, FocusColor, LineThickness);

		// 焦点框旁标最外层实例名；框贴近视口顶部时标签移到框内
		if (State->FocusOwner.Len() > 0)
		{
			const FSlateFontInfo LabelFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
			const FString LabelText = State->FocusOwner;
			// 宽度估算：避免引字体测量服务；调试标签足够
			const FVector2f LabelSize(FMath::Max(40.0f, LabelText.Len() * 7.5f + 10.0f), 18.0f);
			FVector2f LabelPos(FocusLocal.Left, FocusLocal.Top - LabelSize.Y);
			if (LabelPos.Y < 0.0f)
			{
				LabelPos.Y = FocusLocal.Top + 2.0f;
			}

			static FSlateColorBrush LabelBrush(FLinearColor::White);
			const FPaintGeometry LabelBgGeometry = AllottedGeometry.ToPaintGeometry(LabelSize, FSlateLayoutTransform(LabelPos));
			FSlateDrawElement::MakeBox(OutDrawElements, CurrentLayer++, LabelBgGeometry, &LabelBrush, ESlateDrawEffect::None, InfoBgColor);

			const FPaintGeometry LabelTextGeometry = AllottedGeometry.ToPaintGeometry(
				LabelSize, FSlateLayoutTransform(LabelPos + FVector2f(5.0f, 2.0f)));
			FSlateDrawElement::MakeText(OutDrawElements, CurrentLayer++, LabelTextGeometry, LabelText, LabelFont, ESlateDrawEffect::None, TextColor);
		}
	}

	// 3) 信息栏：半透明黑底（焦点信息/所属/hover 信息/hover 所属），行数随内容增减
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
		static FSlateColorBrush BgBrush(FLinearColor::White);
		static FSlateColorBrush DotBrush(FLinearColor::White);
		const FLinearColor OwnerColor(0.55f, 0.75f, 0.95f, 1.0f);

		struct FLine { FString Text; FLinearColor Color; float OffsetX; };
		TArray<FLine, TInlineAllocator<4>> Lines;
		const FString FocusLabel = Tr2(TEXT("焦点"), TEXT("Focus"));
		const FString OwnerLabel = Tr2(TEXT("所属"), TEXT("Owner"));
		const FString CauseNone = Tr2(TEXT("(无)"), TEXT("(none)"));
		Lines.Add({ State->bHasFocus
			? FString::Printf(TEXT("%s  %s · %s · u%d · %s %s"), *FocusLabel, *State->FocusName, *State->FocusType, State->FocusUser,
				Tr2(TEXT("来源"), TEXT("cause")), *CauseDisplay(State->Cause))
			: FString::Printf(TEXT("%s  %s · %s %s"), *FocusLabel, *CauseNone, Tr2(TEXT("来源"), TEXT("cause")), *CauseDisplay(State->Cause)),
			TextColor, State->bMismatch ? 22.0f : 8.0f });
		if (State->FocusOwner.Len() > 0)
		{
			Lines.Add({ FString::Printf(TEXT("%s  %s"), *OwnerLabel, *State->FocusOwner), OwnerColor, 22.0f });
		}
		Lines.Add({ State->bHasHover
			? FString::Printf(TEXT("hover  %s · %s · %s"), *State->HoverName, *State->HoverType, *State->HoverVis)
			: FString::Printf(TEXT("hover  %s"), *CauseNone), TextColor, 8.0f });
		if (State->HoverOwner.Len() > 0)
		{
			Lines.Add({ FString::Printf(TEXT("%s  %s"), *OwnerLabel, *State->HoverOwner), OwnerColor, 22.0f });
		}
		if (State->ActivationText.Len() > 0 || State->InputMethod.Len() > 0)
		{
			FString ActivationLine = Tr2(TEXT("激活  "), TEXT("Active  "));
			ActivationLine += State->ActivationText.Len() > 0 ? State->ActivationText : CauseNone;
			if (State->InputMethod.Len() > 0)
			{
				ActivationLine += FString::Printf(TEXT(" · %s"), *State->InputMethod);
			}
			Lines.Add({ MoveTemp(ActivationLine), OwnerColor, 8.0f });
		}

		constexpr float LineHeight = 21.0f;
		constexpr float PadY = 7.0f;
		// 宽自适应：最长行实测 + 边距；超视口 70% 时按比例保尾截断各行。
		float MaxLineW = 0.0f;
		for (const FLine& L : Lines)
		{
			MaxLineW = FMath::Max(MaxLineW, MeasureW(L.Text, Font));
		}
		float PanelW = MaxLineW + 16.0f;
		if (PanelW > WidthCap)
		{
			const float Scale = (WidthCap - 20.0f) / FMath::Max(MaxLineW, 1.0f);
			for (FLine& L : Lines)
			{
				L.Text = TruncTailN(L.Text, FMath::Max(FMath::FloorToInt32(L.Text.Len() * Scale), 6));
			}
			PanelW = WidthCap;
		}
		const FVector2f PanelSize(PanelW, PadY * 2.0f + Lines.Num() * LineHeight);
		const FPaintGeometry BgPaintGeometry = AllottedGeometry.ToPaintGeometry(PanelSize, FSlateLayoutTransform(InfoPos));

		// 颜色走元素 tint（白画刷 × 颜色 = 颜色），与引擎 SViewport 的 ColorBox 模式同构；
		// 直接把颜色烘进画刷构造的路径实测渲染成白色，弃用。
		FSlateDrawElement::MakeBox(OutDrawElements, CurrentLayer++, BgPaintGeometry, &BgBrush, ESlateDrawEffect::None, InfoBgColor);

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FPaintGeometry LineGeometry = AllottedGeometry.ToPaintGeometry(
				PanelSize - FVector2f(Lines[i].OffsetX + 4.0f, 0.0f),
				FSlateLayoutTransform(InfoPos + FVector2f(Lines[i].OffsetX, PadY + i * LineHeight)));
			FSlateDrawElement::MakeText(OutDrawElements, CurrentLayer++, LineGeometry, Lines[i].Text, Font, ESlateDrawEffect::None, Lines[i].Color);
		}

		if (State->bMismatch)
		{
			const FPaintGeometry DotGeometry = AllottedGeometry.ToPaintGeometry(
				DotSize, FSlateLayoutTransform(InfoPos + FVector2f(DotOffsetX, PadY + 7.0f)));
			FSlateDrawElement::MakeBox(OutDrawElements, CurrentLayer++, DotGeometry, &DotBrush, ESlateDrawEffect::None, MismatchColor);
		}
	}

	// 4) 事件尾巴：右上角最近事件流（随 uifocus.overlay 一起显示，不加独立开关）
	if (State->RecentEvents.Num() > 0)
	{
		const FSlateFontInfo EventFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14);
		static FSlateColorBrush EventBgBrush(FLinearColor::White);
		const FLinearColor NormalEventColor(0.80f, 0.83f, 0.88f, 1.0f);
		const FLinearColor SetDirectlyColor(1.0f, 0.78f, 0.35f, 1.0f);
		const FLinearColor ActivationColor(0.55f, 0.75f, 0.95f, 1.0f);

		// cause 缩写到固定宽度，保证三列对齐
		const auto ShortCause = [](const FString& InCause) -> FString
		{
			if (InCause == TEXT("Navigation"))         return TEXT("Nav");
			if (InCause == TEXT("SetDirectly"))        return TEXT("Set");
			if (InCause == TEXT("OtherWidgetLostFocus")) return TEXT("OtherLost");
			if (InCause == TEXT("WindowActivate"))     return TEXT("Window");
			if (InCause == TEXT("Activated"))          return TEXT("Act");
			if (InCause == TEXT("Deactivated"))        return TEXT("Deact");
			return InCause; // Mouse / Cleared
		};

		// 长名截中段（非名字列文本用），避免行超宽
		const auto TruncMid = [](const FString& In, int32 MaxLen) -> FString
		{
			if (In.Len() <= MaxLen)
			{
				return In;
			}
			return In.Left(MaxLen / 2 - 1) + TEXT("…") + In.Right(MaxLen / 2 - 1);
		};

		constexpr float EventLineHeight = 20.0f;
		constexpr float EventPadY = 8.0f;
		const int32 RowCount = State->RecentEvents.Num();
		const double NewestTimeMs = State->RecentEvents.Last().TimeMs;

		// 先拼全部行（文本+颜色），再按内容测宽——面板宽自适应，宁宽勿截。
		struct FEventRow { FString Text; FLinearColor Color; };
		TArray<FEventRow> EventRows;
		EventRows.Reserve(RowCount);
		for (int32 i = 0; i < RowCount; ++i)
		{
			const FUiFocusEvent& Ev = State->RecentEvents[i];
			const bool bIsNewest = (i == RowCount - 1);
			const int32 AgeFromNewest = RowCount - 1 - i;

			FLinearColor RowColor = NormalEventColor;
			FString Line;
			if (Ev.Type == TEXT("ActivationChanged"))
			{
				Line = FString::Printf(TEXT("%-9s %s"), *ShortCause(Ev.Cause),
					*TruncMid(Ev.NewName.Len() > 0 ? Ev.NewName : Ev.OldName, 40));
				RowColor = ActivationColor;
			}
			else
			{
				// 名字列只显示所属控件（信息栏同源判定）；无 UMG 祖先（纯 Slate 控件）回退控件自身名。
				const FString OldDisplay = Ev.OldOwnerName.Len() > 0 ? Ev.OldOwnerName : Ev.OldName;
				const FString NewDisplay = Ev.NewName.Len() == 0 ? FString(TEXT("∅"))
					: (Ev.NewOwnerName.Len() > 0 ? Ev.NewOwnerName : Ev.NewName);
				Line = FString::Printf(TEXT("%-9s %s → %s"), *ShortCause(Ev.Cause),
					*TruncTailN(OldDisplay, 24), *TruncTailN(NewDisplay, 24));
				if (Ev.Cause == TEXT("Cleared"))
				{
					RowColor = MismatchColor;
				}
				else if (Ev.Cause == TEXT("SetDirectly"))
				{
					RowColor = SetDirectlyColor;
				}
			}
			// 导航决策独立行（导航未导致焦点变化时由模块延迟入队）：`方向 规则@生效控件 → 目的地`
			if (Ev.Type == TEXT("Navigation"))
			{
				const FString RuleAt = Ev.NavHandlerName.Len() > 0
					? FString::Printf(TEXT("%s@%s"), *NavRuleDisplay(Ev.NavRule), *Ev.NavHandlerName)
					: NavRuleDisplay(Ev.NavRule);
				const FString Dest = Ev.NavDestOwnerName.Len() > 0 ? TruncTailN(Ev.NavDestOwnerName, 24)
					: (Ev.NavDestName.Len() > 0 ? TruncTailN(Ev.NavDestName, 24)
						: Tr2(TEXT("(焦点未移动)"), TEXT("(focus unchanged)")));
				Line = FString::Printf(TEXT("%-6s %s → %s"),
					*NavDirectionDisplay(Ev.NavDirection), *TruncMid(RuleAt, 34), *Dest);
				RowColor = NavigationColor;
			}
			// 新旧分级：最新一行 ▶ 前缀 + 提亮；近 3 行原色；更早压暗
			FString Prefix = bIsNewest ? TEXT("▶ ") : TEXT("  ");
			if (bIsNewest)
			{
				RowColor = FLinearColor::White;
			}
			else if (AgeFromNewest > 3)
			{
				RowColor *= FLinearColor(1.0f, 1.0f, 1.0f, 0.55f);
			}

			// 时间列=相对最新事件的秒差（最新行 0.0）：绝对秒（开机起算 8 位）会把行撑爆，秒差也更可读。
			EventRows.Add({ FString::Printf(TEXT("%s%5.1f %s"), *Prefix, (NewestTimeMs - Ev.TimeMs) / 1000.0, *Line), RowColor });
		}

		float MaxEventW = 0.0f;
		for (const FEventRow& Row : EventRows)
		{
			MaxEventW = FMath::Max(MaxEventW, MeasureW(Row.Text, EventFont));
		}
		float EventPanelW = MaxEventW + 16.0f;
		if (EventPanelW > WidthCap)
		{
			const float Scale = (WidthCap - 20.0f) / FMath::Max(MaxEventW, 1.0f);
			for (FEventRow& Row : EventRows)
			{
				Row.Text = TruncTailN(Row.Text, FMath::Max(FMath::FloorToInt32(Row.Text.Len() * Scale), 8));
			}
			EventPanelW = WidthCap;
		}
		const FVector2f EventPanelSize(EventPanelW, EventPadY * 2.0f + EventRows.Num() * EventLineHeight);
		const FVector2f EventPanelPos(LocalSize.X - EventPanelSize.X - 8.0f, 8.0f);

		const FPaintGeometry EventBgGeometry = AllottedGeometry.ToPaintGeometry(EventPanelSize, FSlateLayoutTransform(EventPanelPos));
		FSlateDrawElement::MakeBox(OutDrawElements, CurrentLayer++, EventBgGeometry, &EventBgBrush, ESlateDrawEffect::None, InfoBgColor);

		for (int32 i = 0; i < EventRows.Num(); ++i)
		{
			const FPaintGeometry RowGeometry = AllottedGeometry.ToPaintGeometry(
				EventPanelSize - FVector2f(10.0f, 0.0f),
				FSlateLayoutTransform(EventPanelPos + FVector2f(6.0f, EventPadY + i * EventLineHeight)));
			FSlateDrawElement::MakeText(OutDrawElements, CurrentLayer++, RowGeometry, EventRows[i].Text, EventFont, ESlateDrawEffect::None, EventRows[i].Color);
		}
	}

	// 5) 左下角导航明细：最近一次按键/摇杆导航走了哪条规则、去了哪里，以及链上各容器的配置。
	if (State->bHasNav)
	{
		const FUiFocusEvent& Nav = State->LatestNav;
		const FSlateFontInfo NavFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12);
		static FSlateColorBrush NavBgBrush(FLinearColor::White);

		TArray<FString> Rows;
		// 行1：什么操作。例：导航 右 ← 手柄
		Rows.Add(FString::Printf(TEXT("%s %s ← %s"),
			Tr2(TEXT("导航"), TEXT("Nav")), *NavDirectionDisplay(Nav.NavDirection),
			*NavGenesisDisplay(Nav.NavGenesis)));
		// 行2：哪条规则生效、配在谁身上。例：生效规则 Wrap(循环) @ W_SettingsScreen
		Rows.Add(FString::Printf(TEXT("%s %s @ %s"),
			Tr2(TEXT("生效规则"), TEXT("Rule applied")), *NavRuleDisplay(Nav.NavRule), *Nav.NavHandlerName));
		// 行3：怎么选中的（规则/委托/几何）。例：选取方式 几何推算(无规则命中)
		Rows.Add(FString::Printf(TEXT("%s %s"), Tr2(TEXT("选取方式"), TEXT("Selection")), *NavMethodDisplay(Nav.NavMethod)));
		// 行4：焦点去了哪。例：去向 …_Discrete_C_6(W_SettingsListEntry)
		const FString DestLeaf = Nav.NavDestName.Len() > 0
			? Nav.NavDestName
			: Tr2(TEXT("(焦点未移动)"), TEXT("(focus unchanged)"));
		const FString DestDisplay = Nav.NavDestName.Len() > 0
			? FString(TEXT("…")) + (Nav.NavDestOwnerName.Len() > 0
				? FString::Printf(TEXT("%s(%s)"), *Nav.NavDestName.Right(20), *Nav.NavDestOwnerName.Right(18))
				: Nav.NavDestName.Right(20))
			: DestLeaf;
		Rows.Add(FString::Printf(TEXT("%s %s"), Tr2(TEXT("去向"), TEXT("Target")), *DestDisplay));

		if (Nav.NavRuleChain.Num() > 0)
		{
			Rows.Add(FString::Printf(TEXT("%s:"), Tr2(TEXT("链上配置(内→外)"), TEXT("Chain rules (inner→outer)"))));
			for (const FString& ChainRow : Nav.NavRuleChain)
			{
				// 模块存 "名字|规则token|是否生效"；此处拆开本地化重组。
				FString Name, RuleToken, WinnerFlag;
				const bool bOk = ChainRow.Split(TEXT("|"), &Name, &RuleToken) && RuleToken.Split(TEXT("|"), &RuleToken, &WinnerFlag);
				if (!bOk)
				{
					Rows.Add(FString::Printf(TEXT("  %s"), *ChainRow));
					continue;
				}
				FString Row = FString::Printf(TEXT("  %s: %s"), *Name, *NavRuleDisplay(RuleToken));
				if (WinnerFlag == TEXT("1"))
				{
					Row += Tr2(TEXT("  ◀生效"), TEXT("  ◀applied"));
				}
				Rows.Add(MoveTemp(Row));
			}
		}
		else
		{
			Rows.Add(Tr2(TEXT("链上无显式配置——焦点按位置几何推算"),
				TEXT("No explicit rules on chain — target chosen by geometry")));
		}

		// 行高 22：Bold 12 中文实际渲染高度超出 18px 会上下行重叠（实测截图），22 起留足。
		constexpr float NavLineHeight = 22.0f;
		constexpr float NavPadY = 8.0f;
		const int32 NavRowCount = Rows.Num();
		// 宽自适应：最长行实测 + 边距；超视口 70% 时按比例保尾截断各行。
		float MaxNavW = 0.0f;
		for (const FString& Row : Rows)
		{
			MaxNavW = FMath::Max(MaxNavW, MeasureW(Row, NavFont));
		}
		float NavPanelW = MaxNavW + 16.0f;
		if (NavPanelW > WidthCap)
		{
			const float Scale = (WidthCap - 20.0f) / FMath::Max(MaxNavW, 1.0f);
			for (FString& Row : Rows)
			{
				Row = TruncTailN(Row, FMath::Max(FMath::FloorToInt32(Row.Len() * Scale), 6));
			}
			NavPanelW = WidthCap;
		}
		const FVector2f NavPanelSize(NavPanelW, NavPadY * 2.0f + NavRowCount * NavLineHeight);
		const FVector2f NavPanelPos(8.0f, LocalSize.Y - NavPanelSize.Y - 8.0f);

		const FPaintGeometry NavBgGeometry = AllottedGeometry.ToPaintGeometry(NavPanelSize, FSlateLayoutTransform(NavPanelPos));
		FSlateDrawElement::MakeBox(OutDrawElements, CurrentLayer++, NavBgGeometry, &NavBgBrush, ESlateDrawEffect::None, InfoBgColor);

		for (int32 i = 0; i < NavRowCount; ++i)
		{
			FLinearColor RowColor = NavChainColor;
			if (i == 0 || i == 3)
			{
				RowColor = TextColor;
			}
			else if (i == 1)
			{
				RowColor = NavWinColor;
			}
			const FPaintGeometry RowGeometry = AllottedGeometry.ToPaintGeometry(
				NavPanelSize - FVector2f(10.0f, 0.0f),
				FSlateLayoutTransform(NavPanelPos + FVector2f(6.0f, NavPadY + i * NavLineHeight)));
			FSlateDrawElement::MakeText(OutDrawElements, CurrentLayer++, RowGeometry, Rows[i], NavFont, ESlateDrawEffect::None, RowColor);
		}
	}

	return CurrentLayer;
}

// ---------------- FUiFocusOverlayManager ----------------

namespace
{
	TAutoConsoleVariable<bool> CVarOverlay(TEXT("uifocus.overlay"), false,
		TEXT("在游戏画面内显示焦点调试叠加层：焦点虚线框（失配变红）、hover 蓝框、左上角信息栏（含激活栈）、右上角最近事件尾巴。关闭时零开销。"));
}

void FUiFocusOverlayManager::Startup()
{
	CVarOverlay->SetOnChangedCallback(FConsoleVariableDelegate::CreateRaw(this, &FUiFocusOverlayManager::OnCvarChanged));
	if (CVarOverlay->GetBool())
	{
		StartTicker();
	}
}

void FUiFocusOverlayManager::Shutdown()
{
	StopTicker();
	DestroyOverlay();
	CVarOverlay->SetOnChangedCallback(FConsoleVariableDelegate());
}

void FUiFocusOverlayManager::OnCvarChanged(IConsoleVariable*)
{
	if (CVarOverlay->GetBool())
	{
		StartTicker();
	}
	else
	{
		StopTicker();
		DestroyOverlay();
	}
	// 叠加层开关驱动默认会话记录（uifocus.record=-1 跟随模式；编辑器工具栏按钮走同一 cvar）
	FUiFocusDebuggerModule::Get().SyncSessionRecording();
}

void FUiFocusOverlayManager::StartTicker()
{
	if (bTickerActive)
	{
		return;
	}
	// 0.5s 一次的存在性检查：仅 cvar 开启期间运行；cvar 关闭时无 ticker、无控件、零成本。
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("UiFocusOverlayEnsure"), 0.1f,
		[this](float InDeltaTime)
		{
			return TickEnsure(InDeltaTime);
		});
	bTickerActive = true;
}

void FUiFocusOverlayManager::StopTicker()
{
	if (bTickerActive)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
		bTickerActive = false;
	}
}

bool FUiFocusOverlayManager::TickEnsure(float)
{
	UGameViewportClient* GameViewport = GEngine ? GEngine->GameViewport : nullptr;
	if (!GameViewport)
	{
		return true;
	}
	// 弱引用：PIE 结束视口销毁时控件随之析构，弱引用失效 → 下一次 PIE 由本 ticker 自动重建。
	// 注意：SNew 的临时 TSharedRef 是唯一强引用，必须先用局部强引用接住并交给视口，
	// 再写弱引用；直接赋给 TWeakPtr 会在语句结束时引用计数归零、控件立即析构（第六轮崩溃根因）。
	if (!OverlayWeak.IsValid())
	{
		// Slate 视口未就绪（PIE 启动/关卡过渡）时跳过，等下次 ticker 再试。
		if (!GameViewport->GetGameViewportWidget().IsValid())
		{
			return true;
		}
		TSharedRef<SUiFocusOverlay> NewOverlay = SNew(SUiFocusOverlay);
		GameViewport->AddViewportWidgetContent(NewOverlay, /*ZOrder*/10000);
		OverlayWeak = NewOverlay;
	}
	return true;
}

void FUiFocusOverlayManager::DestroyOverlay()
{
	const TSharedPtr<SUiFocusOverlay> Overlay = OverlayWeak.Pin();
	OverlayWeak = nullptr;
	if (!Overlay.IsValid())
	{
		return;
	}
	UGameViewportClient* GameViewport = GEngine ? GEngine->GameViewport : nullptr;
	if (GameViewport)
	{
		GameViewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
	}
}

bool FUiFocusDebuggerModule::IsOverlayEnabled()
{
	return CVarOverlay->GetBool();
}

void FUiFocusDebuggerModule::SetOverlayEnabled(bool bInEnabled)
{
	// SetByConsole 是最高优先级（CanChange 要求 NewPri >= OldPri），此前被控制台手设过的值也能被覆盖。
	CVarOverlay->Set(bInEnabled ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);
}
