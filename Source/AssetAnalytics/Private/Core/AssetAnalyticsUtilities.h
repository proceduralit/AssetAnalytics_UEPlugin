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
	/** Quotes a CSV field and escapes embedded quotes. */
	inline FString EscapeCsvField(FString Value)
	{
		Value.ReplaceInline(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Value);
	}

	/** Returns an integer as CSV text, or an empty field when unavailable. */
	inline FString CsvNumberOrEmpty(const int32 Value)
	{
		return Value == INDEX_NONE ? FString() : FString::FromInt(Value);
	}

	/** Returns a 64-bit integer as CSV text, or an empty field when unavailable. */
	inline FString CsvNumberOrEmpty(const int64 Value)
	{
		return Value < 0 ? FString() : LexToString(Value);
	}

	/** Returns a decimal as CSV text, or an empty field when unavailable. */
	inline FString CsvNumberOrEmpty(const double Value)
	{
		return Value < 0.0 ? FString() : FString::Printf(TEXT("%.3f"), Value);
	}

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
			const FString ReferencerPath = Referencer.ToString();
			const bool bIsDeveloperAsset =
				ReferencerPath.Equals(TEXT("/Game/Developers"), ESearchCase::IgnoreCase) ||
				ReferencerPath.StartsWith(TEXT("/Game/Developers/"), ESearchCase::IgnoreCase);
			if (!bIsDeveloperAsset) return true;
		}
		return false;
	}
}
}
