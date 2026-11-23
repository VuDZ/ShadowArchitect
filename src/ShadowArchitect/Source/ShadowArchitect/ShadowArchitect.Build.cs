using UnrealBuildTool;

public class ShadowArchitect : ModuleRules
{
    public ShadowArchitect(ReadOnlyTargetRules target) : base(target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "EnhancedInput", "InputCore", "Slate", "SlateCore",
            "FactoryGame", "SML", "DummyHeaders"
        });
    }
}
