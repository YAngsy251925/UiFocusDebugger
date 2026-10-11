using UnrealBuildTool;

public class UiFocusDebuggerCommonUI : ModuleRules
{
	public UiFocusDebuggerCommonUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// 零插件依赖：CommonUI/CommonInput 类型全部经 /Script 反射访问（见模块 cpp 头注释），
		// 满足市场"插件不得依赖任何插件才能编译"规则；模块仅由核心模块在运行时检测到
		// CommonUI 已加载后才手动加载。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"UiFocusDebugger"
		});
	}
}
