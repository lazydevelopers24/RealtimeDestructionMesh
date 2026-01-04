// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class RealtimeDestruction : ModuleRules
{
	public RealtimeDestruction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				ModuleDirectory + "/Public",
				ModuleDirectory + "/Public/Components",
				ModuleDirectory + "/Public/BooleanProcessor",
				ModuleDirectory + "/Public/CellStructure",
				ModuleDirectory + "/Public/Debug",
				ModuleDirectory + "/Public/Rendering",
				ModuleDirectory + "/Public/StructuralIntegrity",
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
				"InputCore",
				"NetCore",

				// Geometry & Mesh
				"GeometryCore",
				"GeometryFramework",
				"GeometryScriptingCore",
				"GeometryAlgorithms",
				"DynamicMesh",
				"MeshDescription",
				"ModelingComponents",

				// Chaos / GeometryCollection
				"GeometryCollectionEngine",
				"Chaos",
				"ChaosSolverEngine",
				"FieldSystemEngine",
				"PhysicsCore",

				// Voronoi
				"Voronoi",

				// Rendering
				"RHI",
				"RenderCore",

				// Niagara
				"Niagara",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"ProceduralMeshComponent",
			}
		);

		// Editor-only modules (for WITH_EDITOR code)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"UnrealEd",
					"FractureEngine",
					"GeometryCollectionEditor",
					"DataflowCore",
				}
			);
		}

		// PostBuild: Copy plugin to LyraStarterGame
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string SourcePath = @"$(ProjectDir)\Plugins\RealtimeDestruction";
			string DestPath = @"$(ProjectDir)\..\LyraStarterGame\Plugins\RealtimeDestruction";

			PostBuildSteps.Add($"xcopy /E /Y /I \"{SourcePath}\\Source\" \"{DestPath}\\Source\\\"");
			PostBuildSteps.Add($"xcopy /Y \"{SourcePath}\\RealtimeDestruction.uplugin\" \"{DestPath}\\\"");
			PostBuildSteps.Add("echo [PostBuild] RealtimeDestruction plugin copied to LyraStarterGame!");
		}
	}
}
