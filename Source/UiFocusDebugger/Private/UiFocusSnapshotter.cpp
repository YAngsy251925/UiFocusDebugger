#include "UiFocusSnapshotter.h"

#include "Components/Widget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"

void FUiFocusSnapshotter::RebuildUmgMap() const
{
	UmgBySlate.Reset();
	for (TObjectIterator<UWidget> It; It; ++It)
	{
		UWidget* Widget = *It;
		if (Widget->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}
		const TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
		if (Cached.IsValid() && !UmgBySlate.Contains(Cached.Get()))
		{
			UmgBySlate.Add(Cached.Get(), Widget);
		}
	}
}

FString FUiFocusSnapshotter::DescribeWidget(const TSharedPtr<SWidget>& InWidget) const
{
	if (!InWidget.IsValid())
	{
		return FString();
	}
	const auto TryCache = [this](SWidget* Target) -> UWidget*
	{
		if (const TWeakObjectPtr<UWidget>* Found = UmgBySlate.Find(Target))
		{
			UWidget* Umg = Found->Get();
			if (Umg && Umg->GetCachedWidget().Get() == Target)
			{
				return Umg;
			}
		}
		return nullptr;
	};

	if (UWidget* Umg = TryCache(InWidget.Get()))
	{
		return Umg->GetName();
	}

	// 未命中：可能新控件刚构建，重建一次缓存再查；仍未命中则为纯 Slate 控件。
	RebuildUmgMap();
	if (UWidget* Umg = TryCache(InWidget.Get()))
	{
		return Umg->GetName();
	}
	return InWidget->GetType().ToString();
}

void FUiFocusSnapshotter::ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets) const
{
	OutUWidgets.Reset();

	// 缓存命中的直接消费（带地址复用校验）；未命中收集进 Missing
	TSet<SWidget*> Missing;
	for (SWidget* Target : InTargets)
	{
		if (!Target || OutUWidgets.Contains(Target))
		{
			continue;
		}
		UWidget* Umg = nullptr;
		if (const TWeakObjectPtr<UWidget>* Found = UmgBySlate.Find(Target))
		{
			Umg = Found->Get();
			if (Umg && Umg->GetCachedWidget().Get() != Target)
			{
				Umg = nullptr; // 旧条目：Slate 控件已析构、地址被复用——视为未命中
			}
		}
		if (Umg)
		{
			OutUWidgets.Add(Target, Umg);
		}
		else
		{
			Missing.Add(Target);
		}
	}
	if (Missing.Num() == 0)
	{
		return;
	}

	// 定向扫描：一次 TObjectIterator 只补缺失项并回填缓存（后续事件零扫描）。
	// 同一 SWidget 被多个 UWidget 映射时取首个命中（与旧全量扫描行为一致）。
	int32 Remaining = Missing.Num();
	for (TObjectIterator<UWidget> It; It && Remaining > 0; ++It)
	{
		UWidget* Widget = *It;
		if (Widget->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}
		SWidget* CachedRaw = Widget->GetCachedWidget().Get();
		if (CachedRaw && Missing.Contains(CachedRaw) && !OutUWidgets.Contains(CachedRaw))
		{
			OutUWidgets.Add(CachedRaw, Widget);
			UmgBySlate.Add(CachedRaw, Widget);
			--Remaining;
		}
	}
}
