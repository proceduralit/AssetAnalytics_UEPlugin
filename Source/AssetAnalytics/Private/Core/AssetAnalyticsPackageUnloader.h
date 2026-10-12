// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UPackage;

namespace AssetAnalytics
{
namespace Core
{
	/** Tracks the packages a report loads from disk and unloads them again after each batch. */
	class FReportPackageUnloader
	{
	public:
		/** Returns every package that is currently loaded. */
		static TSet<UPackage*> GetLoadedPackages();

		/**
		 * Tracks the packages loaded from disk since PackagesBefore was taken.
		 * This includes the dependencies an asset pulls in (materials, textures, skeletons, physics assets).
		 */
		void TrackPackagesLoadedSince(const TSet<UPackage*>& PackagesBefore);

		/** Unloads packages loaded by the report and logs any that remain loaded. */
		void UnloadReportPackages();

		/** Forgets tracked packages without unloading them. */
		void Reset();

	private:
		/** Packages loaded by the report that have not been unloaded yet. */
		TArray<UPackage*> PackagesLoadedForReport;
	};
}
}
