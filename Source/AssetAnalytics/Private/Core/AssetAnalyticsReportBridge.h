// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class IHttpRouter;
struct FHttpRouteHandleInternal;

namespace AssetAnalytics
{
namespace Core
{
	/** Returns the local URL for the Mesh Analytics report. */
	FString GetMeshAnalyticsReportUrl();

	/** Returns the local URL for the Material Analytics report. */
	FString GetMaterialAnalyticsReportUrl();

	/** Serves the report files and handles asset links from the browser. */
	class FAssetAnalyticsReportBridge
	{
	public:
		/** Starts the local report server and registers its routes. */
		void Start();

		/** Removes the report routes without stopping other HTTP listeners. */
		void Stop();

	private:
		/** Routes requests from the local browser report. */
		TSharedPtr<IHttpRouter> ReportHttpRouter;

		/** Registered route that selects an asset in the Content Browser. */
		TSharedPtr<const FHttpRouteHandleInternal> SelectAssetRoute;

		/** Registered routes that serve the report page, its scripts and styles, and the generated JSON reports. */
		TArray<TSharedPtr<const FHttpRouteHandleInternal>> ReportFileRoutes;
	};
}
}
