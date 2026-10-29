// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

using UnrealBuildTool;

public class AssetAnalytics : ModuleRules
{
	public AssetAnalytics(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"UnrealEd",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"DeveloperSettings",
				"AssetRegistry",
				"ContentBrowser",
				"HTTPServer",
				"Json",
				"MaterialShaderQualitySettings",
				"RHI",
				"Sockets",
				"TargetPlatform",
				"ToolMenus",
			}
			);
	}
}
