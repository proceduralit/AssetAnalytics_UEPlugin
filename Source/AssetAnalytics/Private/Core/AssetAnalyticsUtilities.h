// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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

	/** Returns a decimal as CSV text, or an empty field when unavailable. */
	inline FString CsvNumberOrEmpty(const double Value)
	{
		return Value < 0.0 ? FString() : FString::Printf(TEXT("%.3f"), Value);
	}

	/** Converts a configured content folder to a valid Unreal package path. */
	inline bool TryGetPackagePath(const FString& ConfiguredPath, FName& OutPackagePath)
	{
		FString Path = ConfiguredPath;
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (!Path.StartsWith(TEXT("/"))) return false;

		OutPackagePath = FName(*Path);
		return true;
	}
}
}
