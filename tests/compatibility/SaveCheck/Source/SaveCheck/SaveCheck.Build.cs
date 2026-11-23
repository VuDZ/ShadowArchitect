using UnrealBuildTool;
public class SaveCheck : ModuleRules
{
    public SaveCheck(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Projects", "FactoryGame", "DummyHeaders" });
    }
}
