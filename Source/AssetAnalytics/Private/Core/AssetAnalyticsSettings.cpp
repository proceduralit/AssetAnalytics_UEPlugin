// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Core/AssetAnalyticsSettings.h"

/** Puts the settings under Plugins > Asset Analytics in Editor Preferences. */
UAssetAnalyticsSettings::UAssetAnalyticsSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Analytics");
}

#if WITH_EDITOR
/** Returns a readable warning about memory retained by analysed assets in any report. */
FText UAssetAnalyticsSettings::GetSectionDescription() const
{
	return NSLOCTEXT(
		"AssetAnalyticsSettings",
		"AssetLoadingNotice",
		"Unreal Editor cannot always unload assets reliably after analysis. This applies to every Asset Analytics report.\n"
		"To limit memory usage, analyse smaller folders and avoid processing too many memory-heavy assets in one report.");
}
#endif
