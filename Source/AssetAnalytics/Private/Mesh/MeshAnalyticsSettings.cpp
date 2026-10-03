// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Mesh/MeshAnalyticsSettings.h"

/** Puts the settings under Plugins > Asset Analytics in Editor Preferences. */
UMeshAnalyticsSettings::UMeshAnalyticsSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Analytics");
}

#if WITH_EDITOR
/** Returns a readable warning about memory retained by analysed assets. */
FText UMeshAnalyticsSettings::GetSectionDescription() const
{
	return NSLOCTEXT(
		"AssetAnalyticsSettings",
		"AssetLoadingNotice",
		"Unreal Editor cannot always unload assets reliably after analysis.\n"
		"To limit memory usage, analyse smaller folders and avoid processing too many memory-heavy assets in one report.");
}
#endif
