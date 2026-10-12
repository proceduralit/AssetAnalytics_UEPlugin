// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Core/AssetAnalyticsPackageUnloader.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace AssetAnalytics
{
namespace Core
{
	/**
	 * Collects the packages that are loaded right now.
	 * Returns: A snapshot to compare against after loading assets.
	 */
	TSet<UPackage*> FReportPackageUnloader::GetLoadedPackages()
	{
		TSet<UPackage*> LoadedPackages;
		ForEachObjectOfClass(UPackage::StaticClass(), [&LoadedPackages](UObject* Object)
		{
			LoadedPackages.Add(static_cast<UPackage*>(Object));
		}, false);
		return LoadedPackages;
	}

	/**
	 * Adds packages loaded from disk since PackagesBefore to the unload list.
	 * Input: PackagesBefore is a snapshot from GetLoadedPackages.
	 */
	void FReportPackageUnloader::TrackPackagesLoadedSince(const TSet<UPackage*>& PackagesBefore)
	{
		ForEachObjectOfClass(UPackage::StaticClass(), [this, &PackagesBefore](UObject* Object)
		{
			UPackage* Package = static_cast<UPackage*>(Object);
			if (PackagesBefore.Contains(Package)) return;
			if (Package->HasAnyPackageFlags(PKG_CompiledIn) || Package->GetLoadedPath().IsEmpty()) return;
			PackagesLoadedForReport.AddUnique(Package);
		}, false);
	}

	/**
	 * Unloads the tracked packages and clears the list.
	 * Output: Logs a warning for unload errors and for packages that stay loaded.
	 */
	void FReportPackageUnloader::UnloadReportPackages()
	{
		if (PackagesLoadedForReport.Num() == 0) return;

		TArray<FName> PackageNames;
		PackageNames.Reserve(PackagesLoadedForReport.Num());
		for (const UPackage* Package : PackagesLoadedForReport)
		{
			if (Package != nullptr) PackageNames.AddUnique(Package->GetFName());
		}

		// Keep the user's undo history; the report never transacts on the packages it loads.
		UPackageTools::FUnloadPackageParams UnloadParams(PackagesLoadedForReport);
		UnloadParams.bResetTransBuffer = false;
		const bool bPackagesChanged = UPackageTools::UnloadPackages(UnloadParams);
		const FText& UnloadError = UnloadParams.OutErrorMessage;
		PackagesLoadedForReport.Reset();

		TArray<FString> RemainingPackageNames;
		for (const FName PackageName : PackageNames)
		{
			if (FindPackage(nullptr, *PackageName.ToString()) != nullptr)
			{
				RemainingPackageNames.Add(PackageName.ToString());
			}
		}

		if (!UnloadError.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not unload some report packages: %s"), *UnloadError.ToString());
		}
		if (!bPackagesChanged)
		{
			UE_LOG(LogTemp, Warning, TEXT("Asset Analytics did not unload any of the %d report-loaded packages."), PackageNames.Num());
		}
		if (RemainingPackageNames.Num() > 0)
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("Asset Analytics retained %d of %d report-loaded packages after unloading: %s"),
				RemainingPackageNames.Num(),
				PackageNames.Num(),
				*FString::Join(RemainingPackageNames, TEXT(", ")));
		}
	}

	/** Clears the tracked packages without unloading them, ready for a new report. */
	void FReportPackageUnloader::Reset()
	{
		PackagesLoadedForReport.Reset();
	}
}
}
