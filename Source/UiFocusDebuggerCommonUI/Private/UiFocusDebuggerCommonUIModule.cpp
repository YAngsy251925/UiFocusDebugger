// CommonUI 桥接（反射版，零编译期插件依赖）：
// - 市场合规：插件不得依赖任何插件才能编译。本模块不 include CommonUI/CommonInput 头文件、
//   Build.cs 不引用其模块；类经 /Script 包路径反射解析，UFUNCTION（IsActivated/GetCurrentInputType
//   均 BlueprintCallable，引擎头已核实）经 ProcessEvent 调用，返回值由参数区首字段带回。
// - 运行时仍由核心模块检测 CommonUI 模块已加载后才手动加载本模块（LoadingPhase=None），
//   故反射目标必然存在；类/函数句柄启动时解析一次。
// - 数据全部轮询差分取得，不订阅实例级委托，对控件生命周期零风险。

#include "UiFocusDebuggerModule.h"

#include "Blueprint/UserWidget.h"
#include "Engine/LocalPlayer.h"
#include "GenericPlatform/GenericPlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** /Script 反射句柄：启动时解析一次。 */
	struct FUiFocusCommonUIReflection
	{
		UClass* ActivatableClass = nullptr;     // /Script/CommonUI.CommonActivatableWidget
		UClass* InputSubsystemClass = nullptr;  // /Script/CommonInput.CommonInputSubsystem
		UFunction* IsActivatedFn = nullptr;
		UFunction* GetInputTypeFn = nullptr;

		void Init()
		{
			ActivatableClass = LoadObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonActivatableWidget"));
			InputSubsystemClass = LoadObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSubsystem"));
			if (ActivatableClass)
			{
				IsActivatedFn = ActivatableClass->FindFunctionByName(TEXT("IsActivated"));
			}
			if (InputSubsystemClass)
			{
				GetInputTypeFn = InputSubsystemClass->FindFunctionByName(TEXT("GetCurrentInputType"));
			}
		}
	};

	FString InputTypeValueToString(uint8 InValue)
	{
		// 优先反射读真实枚举名；失败回退已知顺序（ECommonInputType：0/1/2 = MouseAndKeyboard/Gamepad/Touch）
		if (UEnum* Enum = FindObject<UEnum>(nullptr, TEXT("/Script/CommonInput.ECommonInputType")))
		{
			const FString Name = Enum->GetNameStringByValue(InValue);
			if (!Name.IsEmpty())
			{
				return Name;
			}
		}
		switch (InValue)
		{
		case 0:  return TEXT("MouseAndKeyboard");
		case 1:  return TEXT("Gamepad");
		case 2:  return TEXT("Touch");
		default: return FString::Printf(TEXT("E%d"), static_cast<int32>(InValue));
		}
	}
}

class FUiFocusDebuggerCommonUIModule : public IModuleInterface, public IUiFocusCommonUISource
{
public:
	virtual void StartupModule() override
	{
		Reflection.Init();
		// 由核心模块 LoadModule 进入，核心必然已加载。
		FUiFocusDebuggerModule& Core = FUiFocusDebuggerModule::Get();
		Core.SetCommonUISource(this);
		// 页面边界判定：CommonUI 的 Activatable 即"页面级组件"（屏幕/布局），叠加层所属判定的边界。
		Core.SetActivatablePredicate([this](const UUserWidget* InWidget)
		{
			return InWidget && Reflection.ActivatableClass && InWidget->IsA(Reflection.ActivatableClass);
		});
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("UiFocusDebugger")))
		{
			FUiFocusDebuggerModule& Core = FUiFocusDebuggerModule::Get();
			Core.SetActivatablePredicate(nullptr);
			Core.SetCommonUISource(nullptr);
		}
	}

	virtual void Poll(TArray<FUiFocusEvent>& OutEvents, TArray<FString>& OutActivated, FString& OutInputMethod) override
	{
		// 1) 收集当前激活的 Activatable 实例：全量 UUserWidget 扫描 + 反射类过滤（无 CommonUI 类型可用）
		TArray<TWeakObjectPtr<UObject>> Current;
		if (Reflection.ActivatableClass && Reflection.IsActivatedFn)
		{
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				UUserWidget* Widget = *It;
				if (Widget->HasAnyFlags(RF_ClassDefaultObject) || !Widget->IsA(Reflection.ActivatableClass))
				{
					continue;
				}
				// IsActivated() 为 BlueprintCallable：ProcessEvent 调用，bool 返回值=参数区首字段
				struct { bool bActivated; } Args { false };
				Widget->ProcessEvent(Reflection.IsActivatedFn, &Args);
				if (Args.bActivated)
				{
					Current.Add(Widget);
					OutActivated.Add(Widget->GetName());
				}
			}
		}

		// 2) 差分出激活/停用事件
		for (const TWeakObjectPtr<UObject>& Weak : Current)
		{
			if (!LastActivated.Contains(Weak))
			{
				FUiFocusEvent& Ev = OutEvents.AddDefaulted_GetRef();
				Ev.TimeMs = FPlatformTime::Seconds() * 1000.0;
				Ev.Type = TEXT("ActivationChanged");
				Ev.Cause = TEXT("Activated");
				Ev.NewName = Weak.IsValid() ? Weak->GetName() : FString();
			}
		}
		for (const TWeakObjectPtr<UObject>& Weak : LastActivated)
		{
			if (!Current.Contains(Weak))
			{
				FUiFocusEvent& Ev = OutEvents.AddDefaulted_GetRef();
				Ev.TimeMs = FPlatformTime::Seconds() * 1000.0;
				Ev.Type = TEXT("ActivationChanged");
				Ev.Cause = TEXT("Deactivated");
				Ev.OldName = Weak.IsValid() ? Weak->GetName() : FString(TEXT("(destroyed)"));
			}
		}
		LastActivated = MoveTemp(Current);

		// 3) 输入方式（第一个本地玩家）：子系统经 GetSubsystemBase（反射类），
		//    GetCurrentInputType 为 BlueprintCallable，uint8 返回值=参数区首字段。
		ULocalPlayer* LocalPlayer = nullptr;
		for (TObjectIterator<ULocalPlayer> It; It; ++It)
		{
			if (!It->HasAnyFlags(RF_ClassDefaultObject))
			{
				LocalPlayer = *It;
				break;
			}
		}
		if (LocalPlayer && Reflection.InputSubsystemClass && Reflection.GetInputTypeFn)
		{
			const TSubclassOf<ULocalPlayerSubsystem> SubsystemClass(Reflection.InputSubsystemClass);
			if (ULocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystemBase(SubsystemClass))
			{
				struct { uint8 InputType; } Args { 0 };
				Subsystem->ProcessEvent(Reflection.GetInputTypeFn, &Args);
				OutInputMethod = InputTypeValueToString(Args.InputType);
			}
		}
	}

private:
	FUiFocusCommonUIReflection Reflection;
	TArray<TWeakObjectPtr<UObject>> LastActivated;
};

IMPLEMENT_MODULE(FUiFocusDebuggerCommonUIModule, UiFocusDebuggerCommonUI)
