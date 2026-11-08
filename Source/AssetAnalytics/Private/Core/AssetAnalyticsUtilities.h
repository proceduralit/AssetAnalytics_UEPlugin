// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"

namespace AssetAnalytics
{
namespace Core
{
	/** Converts a configured, mounted content folder to a valid Unreal package path. */
	inline bool TryGetPackagePath(const FString& ConfiguredPath, FName& OutPackagePath)
	{
		FString Path = ConfiguredPath;
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (!Path.StartsWith(TEXT("/"))) return false;

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Path / TEXT(""), Filename)) return false;

		OutPackagePath = FName(*Path);
		return true;
	}

	/** Returns whether PackageName is inside the project's Developers folder. */
	inline bool IsDeveloperPackage(const FName PackageName)
	{
		const FString PackagePath = PackageName.ToString();
		return PackagePath.Equals(TEXT("/Game/Developers"), ESearchCase::IgnoreCase) ||
			PackagePath.StartsWith(TEXT("/Game/Developers/"), ESearchCase::IgnoreCase);
	}

	/** Returns whether AssetData has a game reference, optionally ignoring the Developers folder. */
	inline bool HasRelevantReferencer(
		IAssetRegistry& AssetRegistry,
		const FAssetData& AssetData,
		const bool bIgnoreDeveloperReferences)
	{
		TArray<FName> GameReferencers;
		AssetRegistry.GetReferencers(
			AssetData.PackageName,
			GameReferencers,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Game));
		if (!bIgnoreDeveloperReferences) return GameReferencers.Num() > 0;

		for (const FName Referencer : GameReferencers)
		{
			if (!IsDeveloperPackage(Referencer)) return true;
		}
		return false;
	}
}
}
