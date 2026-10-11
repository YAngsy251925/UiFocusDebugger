#include "UiFocusDebuggerEditorModule.h"

#include "Framework/Commands/UIAction.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenuEntry.h"
#include "ToolMenus.h"
#include "UiFocusDebuggerModule.h"

#define LOCTEXT_NAMESPACE "UiFocusDebuggerEditor"

void FUiFocusDebuggerEditorModule::StartupModule()
{
	// ToolMenus 就绪早于或晚于本模块加载都能正确处理：内部就绪后立即回调。
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUiFocusDebuggerEditorModule::RegisterMenus));
}

void FUiFocusDebuggerEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);

	// 按注册 owner 注销本模块加的菜单项，避免热重载/插件卸载后悬空。
	UToolMenus::UnregisterOwner(this);
}

void FUiFocusDebuggerEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// PlayToolBar：PIE 播放/停止控件所在工具栏。叠加层只在 PIE 里渲染，按钮放这里最顺手。
	UToolMenu* PlayToolBar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
	FToolMenuSection& Section = PlayToolBar->FindOrAddSection("UiFocusDebugger");

	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"ToggleFocusOverlay",
		FUIAction(
			FExecuteAction::CreateRaw(this, &FUiFocusDebuggerEditorModule::ToggleOverlay),
			FCanExecuteAction::CreateRaw(this, &FUiFocusDebuggerEditorModule::CanToggleOverlay),
			FGetActionCheckState::CreateRaw(this, &FUiFocusDebuggerEditorModule::GetOverlayCheckState)),
		LOCTEXT("ToggleFocusOverlayLabel", "UI Focus"),
		LOCTEXT("ToggleFocusOverlayTooltip", "切换游戏画面内的焦点调试叠加层（等同 uifocus.overlay）。PIE 中立即生效；非 PIE 时设置会保留，下次进入 PIE 自动显示。"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Debug"),
		EUserInterfaceActionType::ToggleButton));
}

void FUiFocusDebuggerEditorModule::ToggleOverlay()
{
	FUiFocusDebuggerModule::SetOverlayEnabled(!FUiFocusDebuggerModule::IsOverlayEnabled());
}

bool FUiFocusDebuggerEditorModule::CanToggleOverlay() const
{
	// 运行时模块是 PostDefault 的 DeveloperTool，正常总在编辑器里；守一层以防模块加载顺序异常。
	return FModuleManager::Get().IsModuleLoaded(TEXT("UiFocusDebugger"));
}

ECheckBoxState FUiFocusDebuggerEditorModule::GetOverlayCheckState() const
{
	return CanToggleOverlay() && FUiFocusDebuggerModule::IsOverlayEnabled()
		? ECheckBoxState::Checked
		: ECheckBoxState::Unchecked;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUiFocusDebuggerEditorModule, UiFocusDebuggerEditor)
