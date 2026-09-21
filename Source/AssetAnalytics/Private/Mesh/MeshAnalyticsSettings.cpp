// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Mesh/MeshAnalyticsSettings.h"

/** Puts the settings under Plugins > Asset Analytics in Editor Preferences. */
UMeshAnalyticsSettings::UMeshAnalyticsSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Analytics");
}
