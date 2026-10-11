#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateTypes.h"

/** 在关卡编辑器 Play 工具栏注册一个切换按钮，程序化翻转 uifocus.overlay（画面内焦点调试叠加层）。 */
class FUiFocusDebuggerEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	void ToggleOverlay();
	bool CanToggleOverlay() const;
	ECheckBoxState GetOverlayCheckState() const;
};
