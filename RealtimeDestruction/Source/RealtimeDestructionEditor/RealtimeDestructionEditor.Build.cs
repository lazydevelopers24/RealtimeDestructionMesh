// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class RealtimeDestructionEditor : ModuleRules
{
	public RealtimeDestructionEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				ModuleDirectory + "/Public",
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				ModuleDirectory + "/Private",
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"ComponentVisualizers",
				"RenderCore",
				"RHI",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"RealtimeDestruction",

				"Slate",
				"SlateCore",
				"PropertyEditor",
				"EditorStyle",

				"GeometryCore",
				"GeometryFramework",
				"DynamicMesh",
				"MeshDescription",
				"MeshConversion",

				"GeometryCollectionEngine",
				"GeometryCollectionEditor",
				"Chaos",
				"FractureEngine",
			}
		);
	}
}
