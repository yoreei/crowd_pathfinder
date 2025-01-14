// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class UE5TopDownARPGEditorTarget : TargetRules
{
	public UE5TopDownARPGEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V2;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_1;
        ExtraModuleNames.Add("UE5TopDownARPG");
        // DefaultBuildSettings = BuildSettingsVersion.V4;
        // IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        // ExtraModuleNames.AddRange(new string[] { "UE5TopDownARPG", "CrowdPF" });
    }
}
