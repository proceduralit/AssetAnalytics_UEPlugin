// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mesh/MeshAnalyticsSettings.h"

/** Puts the settings under Plugins > Asset Analytics in Editor Preferences. */
UMeshAnalyticsSettings::UMeshAnalyticsSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Analytics");
}
