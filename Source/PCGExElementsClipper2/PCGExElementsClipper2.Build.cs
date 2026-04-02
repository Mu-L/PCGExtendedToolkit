// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/
// Originally ported from cavalier_contours by jbuckmccready (https://github.com/jbuckmccready/cavalier_contours)

using System;
using System.IO;
using UnrealBuildTool;

public class PCGExElementsClipper2 : ModuleRules
{
	public PCGExElementsClipper2(ReadOnlyTargetRules Target) : base(Target)
	{
		bool bNoPCH = Environment.GetEnvironmentVariable("PCGEX_NO_PCH") == "1" || File.Exists(Path.Combine(ModuleDirectory, "..", "..", "Config", ".noPCH")); 
		PCHUsage = bNoPCH ? PCHUsageMode.NoPCHs : PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = true;
		MinSourceFilesForUnityBuildOverride = 4;
		PrecompileForTargets = PrecompileTargetsType.Any;
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine", 
				"PCG",
				"PCGExCore",
				"PCGExBlending",
				"PCGExFilters",
				"PCGExMatching",
				"PCGExFoundations",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
