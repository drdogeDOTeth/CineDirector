// Copyright Roundtree. All Rights Reserved.

using UnrealBuildTool;

public class CineDirector : ModuleRules
{
	public CineDirector(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "UnrealEd",
			"EditorFramework", "ToolMenus", "WorkspaceMenuStructure", "Projects",
			"MovieScene", "MovieSceneTracks", "LevelSequence", "LevelSequenceEditor",
			"Sequencer", "CinematicCamera", "MovieRenderPipelineCore",
			"MovieRenderPipelineEditor", "MovieRenderPipelineRenderPasses",
			"MovieRenderPipelineSettings", "AssetTools", "AssetRegistry",
			"ContentBrowser", "IKRig", "IKRigEditor", "DesktopPlatform",
		});
	}
}
