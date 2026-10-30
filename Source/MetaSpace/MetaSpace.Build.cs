// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MetaSpace : ModuleRules
{
	public MetaSpace(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"HTTP",
			"Json",
			"JsonUtilities"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("sapi.lib");
			PublicSystemLibraries.Add("ole32.lib");
		}

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"MetaSpace",
			"MetaSpace/Variant_Platforming",
			"MetaSpace/Variant_Platforming/Animation",
			"MetaSpace/Variant_Combat",
			"MetaSpace/Variant_Combat/AI",
			"MetaSpace/Variant_Combat/Animation",
			"MetaSpace/Variant_Combat/Gameplay",
			"MetaSpace/Variant_Combat/Interfaces",
			"MetaSpace/Variant_Combat/UI",
			"MetaSpace/Variant_SideScrolling",
			"MetaSpace/Variant_SideScrolling/AI",
			"MetaSpace/Variant_SideScrolling/Gameplay",
			"MetaSpace/Variant_SideScrolling/Interfaces",
			"MetaSpace/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
