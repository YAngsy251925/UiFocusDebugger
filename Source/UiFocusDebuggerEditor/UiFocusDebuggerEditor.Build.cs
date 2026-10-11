using UnrealBuildTool;

public class UiFocusDebuggerEditor : ModuleRules
{
	public UiFocusDebuggerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"ToolMenus",
			"EditorStyle",
			"UiFocusDebugger"
		});
	}
}
