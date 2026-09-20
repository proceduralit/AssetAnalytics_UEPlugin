// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IHttpRouter;
struct FHttpRouteHandleInternal;

namespace AssetAnalytics
{
namespace Core
{
	/** Returns the local URL for the Mesh Analytics report. */
	FString GetAssetAnalyticsReportUrl();

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

		/** Registered route that serves the report HTML page. */
		TSharedPtr<const FHttpRouteHandleInternal> ReportPageRoute;

		/** Registered route that serves the report stylesheet. */
		TSharedPtr<const FHttpRouteHandleInternal> ReportStylesRoute;

		/** Registered route that serves the report application script. */
		TSharedPtr<const FHttpRouteHandleInternal> ReportScriptRoute;

		/** Registered route that serves the local Plotly script. */
		TSharedPtr<const FHttpRouteHandleInternal> ReportPlotlyRoute;

		/** Registered route that serves the generated Mesh Analytics CSV. */
		TSharedPtr<const FHttpRouteHandleInternal> ReportCsvRoute;
	};
}
}
