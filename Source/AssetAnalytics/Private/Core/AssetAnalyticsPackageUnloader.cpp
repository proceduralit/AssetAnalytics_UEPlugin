// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Core/AssetAnalyticsPackageUnloader.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/MetaData.h"
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
			// FileName is set when a package is loaded from disk, so packages created in memory are skipped.
			if (Package->HasAnyPackageFlags(PKG_CompiledIn) || Package->FileName.IsNone()) return;
			PackagesLoadedForReport.AddUnique(Package);
		}, false);
	}

	/**
	 * Unloads Packages with a single garbage collection. It follows UE 4.27's UPackageTools::UnloadPackages, but
	 * that function runs a full garbage collection and ResetLoaders per package, which takes hours for the tens of
	 * thousands of packages a report batch can load. UE 5 prepares every package and collects once, as here.
	 * Packages with unsaved changes and packages holding a world are left loaded.
	 * Output: OutErrorMessage lists the packages with unsaved changes that were left loaded.
	 * Returns: Whether any package was unloaded.
	 */
	static bool UnloadPackagesWithOneGarbageCollection(const TArray<UPackage*>& Packages, FText& OutErrorMessage)
	{
		TArray<UPackage*> PackagesToUnload;
		TArray<FString> DirtyPackageNames;
		for (UPackage* Package : Packages)
		{
			if (Package == nullptr) continue;
			if (Package->IsDirty())
			{
				DirtyPackageNames.Add(Package->GetName());
				continue;
			}
			// The report never needs to unload a level; the engine's version would replace the editor world.
			if (UWorld::FindWorldInPackage(Package) != nullptr) continue;
			PackagesToUnload.AddUnique(Package);
		}
		if (DirtyPackageNames.Num() > 0)
		{
			OutErrorMessage = FText::FromString(FString::Printf(
				TEXT("These packages have unsaved changes and were left loaded: %s"),
				*FString::Join(DirtyPackageNames, TEXT(", "))));
		}
		if (PackagesToUnload.Num() == 0) return false;

		// Complete any load or streaming request first, as the engine does before unloading.
		FlushAsyncLoading();
		(*GFlushStreamingFunc)();
		GEditor->GetSelectedObjects()->DeselectAll();

		// Clear RF_Standalone so the packages' assets can be collected; remember which objects had it.
		TArray<UObject*> ObjectsThatHadStandaloneCleared;
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		bool bScriptPackageUnloaded = false;
		for (UPackage* Package : PackagesToUnload)
		{
			ForEachObjectWithPackage(Package, [AssetEditorSubsystem, &ObjectsThatHadStandaloneCleared](UObject* Object)
			{
				if (Object->IsAsset()) AssetEditorSubsystem->CloseAllEditorsForAsset(Object);
				if (Object->HasAnyFlags(RF_Standalone))
				{
					Object->ClearFlags(RF_Standalone);
					ObjectsThatHadStandaloneCleared.Add(Object);
				}
				return true;
			}, false);

			// Package metadata keeps its package alive through RF_Standalone, so it is cleared too.
			if (Package->MetaData != nullptr && Package->MetaData->HasAnyFlags(RF_Standalone))
			{
				Package->MetaData->ClearFlags(RF_Standalone);
				ObjectsThatHadStandaloneCleared.Add(Package->MetaData);
			}

			Package->bHasBeenFullyLoaded = false;
			Package->ClearFlags(RF_WasLoaded);
			if (Package->HasAnyPackageFlags(PKG_ContainsScript)) bScriptPackageUnloaded = true;
			// ResetLoaders is deliberately not called: it reads all of the package's bulk data (texture and mesh
			// source) from disk before detaching the file, which takes minutes for thousands of packages. Packages
			// that the collection deletes detach their file in UPackage::BeginDestroy without reading it, and
			// packages that survive keep their file attached, like any other loaded asset.
		}

		// Objects that something else still references survive the collection and get RF_Standalone back,
		// as UPackageTools::RestoreStandaloneOnReachableObjects does.
		const FDelegateHandle ReachabilityHandle = FCoreUObjectDelegates::PostReachabilityAnalysis.AddLambda(
			[&ObjectsThatHadStandaloneCleared]()
			{
				for (UObject* Object : ObjectsThatHadStandaloneCleared)
				{
					if (!Object->IsUnreachable()) Object->SetFlags(RF_Standalone);
				}
			});

		// Names are kept because the collection deletes the unloaded package objects.
		TArray<FString> PackageNamesToUnload;
		PackageNamesToUnload.Reserve(PackagesToUnload.Num());
		for (const UPackage* Package : PackagesToUnload)
		{
			PackageNamesToUnload.Add(Package->GetName());
		}
		PackagesToUnload.Reset();

		// Unloading may invalidate render resources.
		FlushRenderingCommands();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(ReachabilityHandle);

		// Packages that survived may have been marked dirty by callbacks during the collection.
		for (const FString& PackageName : PackageNamesToUnload)
		{
			UPackage* SurvivingPackage = FindPackage(nullptr, *PackageName);
			if (SurvivingPackage != nullptr && SurvivingPackage->IsDirty()) SurvivingPackage->SetDirtyFlag(false);
		}

		if (bScriptPackageUnloaded) GEditor->BroadcastClassPackageLoadedOrUnloaded();
		return true;
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

		// The report never transacts on the packages it loads, so the user's undo history is kept.
		FText UnloadError;
		const bool bPackagesChanged = UnloadPackagesWithOneGarbageCollection(PackagesLoadedForReport, UnloadError);
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
