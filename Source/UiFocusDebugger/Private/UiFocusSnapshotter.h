#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h" // TWeakObjectPtr 成员需要 FWeakObjectPtr 完整（裸编译无传递包含）

class SWidget;
class UWidget;

/**
 * UMG 名/UWidget 反查缓存：SWidget→UWidget（事件侧显示名与所属解析共用同一份缓存）。
 * 键为 raw SWidget*，值 TWeakObjectPtr；命中时校验 UWidget 的 GetCachedWidget 仍指向本键，
 * 杜绝 Slate 控件析构后地址复用造成的假命中。未命中按需重建（新控件出现后的首个查询摊销成本）。
 * 所有入口仅 game thread。
 */
class FUiFocusSnapshotter
{
public:
	FString DescribeWidget(const TSharedPtr<SWidget>& InWidget) const;

	/**
	 * 一批 SWidget* 的 UWidget 反查：缓存命中的直接取；未命中的做一次 TObjectIterator
	 * 定向扫描（只补缺失项）并回填缓存。OutUWidgets 键=输入指针，值=对应 UWidget。
	 */
	void ResolveUWidgets(const TArray<SWidget*>& InTargets, TMap<SWidget*, UWidget*>& OutUWidgets) const;

private:
	void RebuildUmgMap() const;

	mutable TMap<SWidget*, TWeakObjectPtr<UWidget>> UmgBySlate;
};
