// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace AssetAnalytics
{
namespace Mesh
{
	/** Gathers mesh data and opens the finished report in a browser. */
	void RunMeshAnalyticsReport();

	/** Stops the current Mesh Analytics report, if there is one. */
	void StopMeshAnalyticsReport();
}
}
