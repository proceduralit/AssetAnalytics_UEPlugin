// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Mesh/MeshAnalytics.h"
#include "Core/AssetAnalyticsSettings.h"
#include "Core/AssetAnalyticsPackageUnloader.h"
#include "Core/AssetAnalyticsProgressNotification.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ProfilingDebugging/ResourceSize.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "MeshAnalytics"

namespace AssetAnalytics
{
namespace Mesh
{
	struct FMeshAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMeshAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		FAssetData AssetData;
		bool bIsSkeletalMesh = false;
		int64 PackageDiskSize = -1;
		int64 PhysicsSize = -1;
		int32 LODCount = INDEX_NONE;
		FString LODGroup = TEXT("None");
		FString CollisionComplexity;
		int32 SimpleCollisionPrimitives = INDEX_NONE;
		int32 ComplexCollisionVertices = INDEX_NONE;
		int32 TriangleCount = INDEX_NONE;
		bool bNaniteEnabled = false;
		int32 NaniteTriangles = 0;
		double NaniteFallbackPercent = 0.0;
		int32 MaterialSlotCount = INDEX_NONE;
		int32 TextureCount = INDEX_NONE;
		int32 MaxTextureResolution = INDEX_NONE;
		int32 UVChannelCount = INDEX_NONE;
		int32 LightmapResolution = INDEX_NONE;
		double MaxBoundsLengthM = -1.0;
	};

	/** Fills Item with the material and texture data enabled in Settings. */
	static void GatherMaterialAnalyticsData(
		const TArray<UMaterialInterface*>& Materials,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bMaterialCount) Item.MaterialSlotCount = Materials.Num();

		const bool bCollectTextureCount = Settings.bTextureCount;
		const bool bCollectTextureResolution = Settings.bMaxTextureResolution;
		if (!bCollectTextureCount && !bCollectTextureResolution) return;

		TSet<UTexture*> UniqueTextures;
		for (const UMaterialInterface* Material : Materials)
		{
			if (Material == nullptr) continue;

			TArray<UTexture*> MaterialTextures;
			Material->GetUsedTextures(MaterialTextures);
			for (UTexture* Texture : MaterialTextures)
			{
				if (Texture != nullptr) UniqueTextures.Add(Texture);
			}
		}
		Item.TextureCount = UniqueTextures.Num();

		if (!bCollectTextureResolution) return;
		int32 MaxTextureResolution = 0;
		for (const UTexture* Texture : UniqueTextures)
		{
			// Use the imported size for 2D textures, capped by Maximum Texture Size when set.
			int32 TextureResolution = FMath::RoundToInt(
				FMath::Max(Texture->GetSurfaceWidth(), Texture->GetSurfaceHeight()));
			if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				const FIntPoint ImportedSize = Texture2D->GetImportedSize();
				TextureResolution = FMath::Max(ImportedSize.X, ImportedSize.Y);
			}
			if (Texture->MaxTextureSize > 0) TextureResolution = FMath::Min(TextureResolution, Texture->MaxTextureSize);

			MaxTextureResolution = FMath::Max(MaxTextureResolution, TextureResolution);
		}
		Item.MaxTextureResolution = MaxTextureResolution;
	}

	/** Fills Item with the enabled skeletal mesh data. */
	static void GatherSkeletalMeshData(
		USkeletalMesh& SkeletalMesh,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		Item.bIsSkeletalMesh = true;
		if (Settings.bLODGroup) Item.LODGroup = TEXT("None");
		if (Settings.bLODCount) Item.LODCount = SkeletalMesh.GetLODNum();
		if (Settings.bPhysicsSize && Item.PhysicsSize < 0)
		{
			Item.PhysicsSize = 0;
			if (UBodySetup* MeshBodySetup = SkeletalMesh.GetBodySetup())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				MeshBodySetup->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize += static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
			if (UPhysicsAsset* PhysicsAsset = SkeletalMesh.GetPhysicsAsset())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				PhysicsAsset->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize += static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
		}

		const bool bGatherMaterialData =
			Settings.bMaterialCount ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution;
		if (bGatherMaterialData)
		{
			TArray<UMaterialInterface*> Materials;
			for (const FSkeletalMaterial& SkeletalMaterial : SkeletalMesh.GetMaterials())
			{
				Materials.Add(SkeletalMaterial.MaterialInterface);
			}
			GatherMaterialAnalyticsData(Materials, Settings, Item);
		}
		if (Settings.bMaxBoundsLength)
		{
			const FVector BoundsSize = SkeletalMesh.GetBounds().BoxExtent * 2.0f;
			Item.MaxBoundsLengthM = FMath::Max(BoundsSize.GetMax(), 1.0f) / 100.0;
		}
		if (Settings.bUVChannelCount)
		{
			const FSkeletalMeshRenderData* RenderData = SkeletalMesh.GetResourceForRendering();
			if (RenderData != nullptr && RenderData->LODRenderData.Num() > 0)
			{
				const FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[0];
				Item.UVChannelCount = static_cast<int32>(LODRenderData.GetNumTexCoords());
			}
		}
	}

	/** Fills Item with the enabled static mesh data. */
	static void GatherStaticMeshData(
		UStaticMesh& StaticMesh,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bLODCount) Item.LODCount = StaticMesh.GetNumLODs();
		if (Settings.bLODGroup) Item.LODGroup = StaticMesh.GetLODGroup().ToString();
		if (Settings.bPhysicsSize && Item.PhysicsSize < 0)
		{
			Item.PhysicsSize = 0;
			if (UBodySetup* BodySetup = StaticMesh.GetBodySetup())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				BodySetup->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize = static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
		}
		if (Settings.bCollisionInfo)
		{
			if (const UBodySetup* BodySetup = StaticMesh.GetBodySetup())
			{
				ECollisionTraceFlag CollisionTraceFlag = BodySetup->CollisionTraceFlag;
				if (CollisionTraceFlag == CTF_UseDefault) CollisionTraceFlag = UPhysicsSettings::Get()->DefaultShapeComplexity;

				switch (CollisionTraceFlag)
				{
				case CTF_UseSimpleAndComplex:
					Item.CollisionComplexity = TEXT("SimpleAndComplex");
					break;
				case CTF_UseSimpleAsComplex:
					Item.CollisionComplexity = TEXT("SimpleAsComplex");
					break;
				case CTF_UseComplexAsSimple:
					Item.CollisionComplexity = TEXT("ComplexAsSimple");
					break;
				default:
					Item.CollisionComplexity = TEXT("None");
					break;
				}
				Item.SimpleCollisionPrimitives = BodySetup->AggGeom.GetElementCount();
			}
			else
			{
				Item.CollisionComplexity = TEXT("None");
				Item.SimpleCollisionPrimitives = 0;
			}

		}
		if (Settings.bComplexCollisionInfo)
		{
			FTriMeshCollisionData CollisionData;
			Item.ComplexCollisionVertices = StaticMesh.GetPhysicsTriMeshData(&CollisionData, false)
				? CollisionData.Vertices.Num()
				: 0;
		}

		const bool bGatherMaterialData =
			Settings.bMaterialCount ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution;
		if (bGatherMaterialData)
		{
			TArray<UMaterialInterface*> Materials;
			for (const FStaticMaterial& StaticMaterial : StaticMesh.GetStaticMaterials())
			{
				Materials.Add(StaticMaterial.MaterialInterface);
			}
			GatherMaterialAnalyticsData(Materials, Settings, Item);
		}
		if (Settings.bMaxBoundsLength)
		{
			const FVector BoundsSize = StaticMesh.GetBounds().BoxExtent * 2.0f;
			Item.MaxBoundsLengthM = FMath::Max(BoundsSize.GetMax(), 1.0f) / 100.0;
		}
		if (Settings.bLightmapResolution) Item.LightmapResolution = StaticMesh.GetLightMapResolution();
	}

	/** Fills Item with values cached in the Asset Registry. */
	static void GatherAssetRegistryData(
		IAssetRegistry& AssetRegistry,
		const FAssetData& AssetData,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		Item.bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		if (Settings.bPackageDiskSize)
		{
			FAssetPackageData PackageData;
			if (AssetRegistry.TryGetAssetPackageData(AssetData.PackageName, PackageData) ==
				UE::AssetRegistry::EExists::Exists)
			{
				Item.PackageDiskSize = PackageData.DiskSize;
			}
		}
		if (Settings.bPhysicsSize) AssetData.GetTagValue(TEXT("PhysicsSize"), Item.PhysicsSize);
		if (Settings.bLODCount) AssetData.GetTagValue(TEXT("LODs"), Item.LODCount);
		if (Settings.bTriangleCount) AssetData.GetTagValue(TEXT("Triangles"), Item.TriangleCount);
		if (Settings.bNaniteInfo)
		{
			AssetData.GetTagValue(TEXT("NaniteEnabled"), Item.bNaniteEnabled);
			AssetData.GetTagValue(TEXT("NaniteTriangles"), Item.NaniteTriangles);
			if (!Item.bIsSkeletalMesh)
			{
				AssetData.GetTagValue(TEXT("NaniteFallbackPercent"), Item.NaniteFallbackPercent);
			}
		}

		if (Item.bIsSkeletalMesh)
		{
			if (Settings.bLODGroup) Item.LODGroup = TEXT("None");
			return;
		}

		if (Settings.bLODGroup) AssetData.GetTagValue(TEXT("LODGroup"), Item.LODGroup);
		if (Settings.bMaterialCount) AssetData.GetTagValue(TEXT("Materials"), Item.MaterialSlotCount);
		if (Settings.bUVChannelCount) AssetData.GetTagValue(TEXT("UVChannels"), Item.UVChannelCount);
		if (Settings.bCollisionInfo)
		{
			AssetData.GetTagValue(TEXT("CollisionComplexity"), Item.CollisionComplexity);
			Item.CollisionComplexity.RemoveFromStart(TEXT("CTF_Use"));
			AssetData.GetTagValue(TEXT("CollisionPrims"), Item.SimpleCollisionPrimitives);
		}
		if (Settings.bMaxBoundsLength)
		{
			FString ApproximateSize;
			if (AssetData.GetTagValue(TEXT("ApproxSize"), ApproximateSize))
			{
				TArray<FString> Dimensions;
				ApproximateSize.ParseIntoArray(Dimensions, TEXT("x"));
				if (Dimensions.Num() == 3)
				{
					const double MaxBoundsLengthCm = FMath::Max3(
						FCString::Atod(*Dimensions[0]),
						FCString::Atod(*Dimensions[1]),
						FCString::Atod(*Dimensions[2]));
					Item.MaxBoundsLengthM = FMath::Max(MaxBoundsLengthCm, 1.0) / 100.0;
				}
			}
		}
	}

	/** Returns whether the enabled columns require loading AssetData. */
	static bool NeedsAssetLoading(
		const FAssetData& AssetData,
		const UAssetAnalyticsSettings& Settings)
	{
		const bool bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		int64 CachedPhysicsSize = -1;
		if (Settings.bPhysicsSize && !AssetData.GetTagValue(TEXT("PhysicsSize"), CachedPhysicsSize)) return true;
		if (Settings.bTextureCount || Settings.bMaxTextureResolution) return true;
		if (bIsSkeletalMesh)
		{
			return Settings.bMaterialCount || Settings.bUVChannelCount || Settings.bMaxBoundsLength;
		}
		return Settings.bComplexCollisionInfo || Settings.bLightmapResolution;
	}

	/**
	 * Fills a report row from registry data and an optional loaded MeshAsset.
	 * Returns: The completed report row.
	 */
	static FMeshAssetData MakeMeshAssetData(
		IAssetRegistry& AssetRegistry,
		const FAssetData& AssetData,
		UObject* MeshAsset,
		const UAssetAnalyticsSettings& Settings)
	{
		FMeshAssetData Item(AssetData);
		GatherAssetRegistryData(AssetRegistry, AssetData, Settings, Item);
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(MeshAsset)) GatherSkeletalMeshData(*SkeletalMesh, Settings, Item);
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(MeshAsset)) GatherStaticMeshData(*StaticMesh, Settings, Item);
		return Item;
	}

	class FMeshAnalyticsRunner
	{
	public:
		/** Starts gathering unless a report is already running. */
		void Start()
		{
			if (TickerHandle.IsValid()) return;
			ResetReportData();
			PackageUnloader.Reset();
			ProgressNotification.Show(
				LOCTEXT("PreparingMeshAnalytics", "Preparing Mesh Analytics..."),
				LOCTEXT("CancelMeshAnalyticsTooltip", "Stop gathering mesh analytics."),
				FSimpleDelegate::CreateRaw(this, &FMeshAnalyticsRunner::Cancel));

			// Work is split into small steps run on separate ticks, so the editor repaints the notification in between.
			Step = ERunnerStep::ShowNotification;
			StepStartSeconds = FPlatformTime::Seconds();
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMeshAnalyticsRunner::Tick));
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
			ProgressNotification.Dismiss();
		}

	private:
		/** Stages of a report run. Each Tick performs one stage, or part of ProcessMeshes. */
		enum class ERunnerStep : uint8
		{
			/** Waits briefly so the notification is visible before any blocking work. */
			ShowNotification,
			/** Scans the search folders and lists the mesh assets. */
			FindMeshes,
			/** Fills report rows within a per-tick time budget. */
			ProcessMeshes,
			/** Unloads the packages loaded by the finished batch. */
			UnloadPackages
		};

		/** Updates the notification with the current asset count and the last asset processed. */
		void UpdateProgressNotification() const
		{
			const int32 ProgressPercent = PendingAssets.Num() > 0
				? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
				: 100;
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("MeshAnalyticsProgress", "Gathering Mesh Analytics - {0} / {1} ({2}%)\n{3}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(ProgressPercent),
				FText::FromString(CurrentAssetName)));
		}

		/** Clears pending assets, finished rows and the batch state. */
		void ResetReportData()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			NextAssetIndex = 0;
			AssetsInBatch = 0;
			bBatchLoadedAnyAsset = false;
			PackagesBeforeBatch.Reset();
			CurrentAssetName.Reset();
		}

		/** Adds the packages loaded by a batch that was interrupted to the unloader, so they are not left loaded. */
		void TrackUnfinishedBatch()
		{
			if (AssetsInBatch > 0 && bBatchLoadedAnyAsset) PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
			ProgressNotification.Finish(
				LOCTEXT("MeshAnalyticsCancelled", "Mesh Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/** Scans the configured folders and lists their mesh assets in PendingAssets. */
		void FindMeshAssets(IAssetRegistry& AssetRegistry)
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FARFilter Filter;
			if (Settings->bStaticMeshes) Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
			if (Settings->bSkeletalMeshes) Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			for (const FDirectoryPath& Directory : Settings->SearchFolders)
			{
				FName PackagePath;
				if (Core::TryGetPackagePath(Directory.Path, PackagePath))
				{
					Filter.PackagePaths.AddUnique(PackagePath);
				}
				else
				{
					UE_LOG(
						LogTemp,
						Warning,
						TEXT("Asset Analytics skipped search folder '%s' because it is invalid or its content root is not mounted."),
						*Directory.Path);
				}
			}
			if (Filter.ClassPaths.Num() == 0 || Filter.PackagePaths.Num() == 0) return;

			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);
		}

		/**
		 * Fills the report row of one mesh asset, loading it only for columns that require it.
		 * Returns: True when this call loaded the asset from disk.
		 */
		bool ProcessMeshAsset(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings, const FAssetData& AssetData)
		{
			if (Settings.bIgnoreUnreferencedAssets &&
				!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings.bIgnoreDeveloperReferences)) return false;

			UObject* MeshAsset = nullptr;
			bool bWasAssetLoaded = true;
			if (NeedsAssetLoading(AssetData, Settings))
			{
				bWasAssetLoaded = AssetData.IsAssetLoaded();
				MeshAsset = AssetData.GetAsset();
			}
			AssetItems.Add(MakeMeshAssetData(AssetRegistry, AssetData, MeshAsset, Settings));
			return !bWasAssetLoaded && MeshAsset != nullptr;
		}

		/**
		 * Processes mesh assets until the tick time budget is used, so the editor keeps repainting.
		 * AssetsPerTick assets make one batch; when a batch has loaded assets, the next tick unloads them.
		 * Returns: True while the report is still running.
		 */
		bool ProcessMeshesForOneTick(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			const int32 AssetsPerBatch = FMath::Max(Settings.AssetsPerTick, 1);
			while (NextAssetIndex < PendingAssets.Num())
			{
				// Snapshot loaded packages so everything this batch loads, including dependencies, can be unloaded afterwards.
				if (AssetsInBatch == 0)
				{
					PackagesBeforeBatch = Core::FReportPackageUnloader::GetLoadedPackages();
					bBatchLoadedAnyAsset = false;
				}

				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				CurrentAssetName = AssetData.AssetName.ToString();
				if (ProcessMeshAsset(AssetRegistry, Settings, AssetData)) bBatchLoadedAnyAsset = true;
				++NextAssetIndex;
				++AssetsInBatch;

				const bool bBatchFinished = AssetsInBatch >= AssetsPerBatch || NextAssetIndex >= PendingAssets.Num();
				if (bBatchFinished)
				{
					if (bBatchLoadedAnyAsset)
					{
						// Unloading can take a while, so it gets its own tick and message.
						PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
						Step = ERunnerStep::UnloadPackages;
						ProgressNotification.SetText(FText::Format(
							LOCTEXT("UnloadingMeshPackages", "Mesh Analytics - {0} / {1}\nUnloading {2} packages..."),
							FText::AsNumber(NextAssetIndex),
							FText::AsNumber(PendingAssets.Num()),
							FText::AsNumber(PackageUnloader.GetTrackedPackageCount())));
						return true;
					}
					AssetsInBatch = 0;
				}
				if (FPlatformTime::Seconds() - TickStartSeconds >= TickTimeBudgetSeconds) break;
			}

			if (NextAssetIndex >= PendingAssets.Num())
			{
				TickerHandle.Reset();
				CompleteReport();
				return false;
			}
			UpdateProgressNotification();
			return true;
		}

		/**
		 * Runs the current stage of the report and moves to the next one. Stages that block the editor run on their
		 * own tick after their message is shown, so the notification always says what is happening.
		 * Returns: True while the report is still running.
		 */
		bool Tick(float)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();

			switch (Step)
			{
			case ERunnerStep::ShowNotification:
				// The notification fades in over a few frames; blocking work before that would hide it.
				if (FPlatformTime::Seconds() - StepStartSeconds < NotificationDisplayDelaySeconds) return true;
				Step = ERunnerStep::FindMeshes;
				ProgressNotification.SetText(LOCTEXT("ScanningMeshFolders", "Mesh Analytics\nScanning mesh folders..."));
				return true;

			case ERunnerStep::FindMeshes:
				FindMeshAssets(AssetRegistry);
				if (PendingAssets.Num() == 0)
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMeshes;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::ProcessMeshes:
				return ProcessMeshesForOneTick(AssetRegistry, *Settings);

			case ERunnerStep::UnloadPackages:
				PackageUnloader.UnloadReportPackages();
				AssetsInBatch = 0;
				bBatchLoadedAnyAsset = false;
				if (NextAssetIndex >= PendingAssets.Num())
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMeshes;
				UpdateProgressNotification();
				return true;
			}
			return true;
		}

		/** Sorts the results, saves the CSV, and opens the browser report. */
		void CompleteReport()
		{
			AssetItems.Sort([](const FMeshAssetData& Left, const FMeshAssetData& Right)
			{
				const FString LeftName = Left.AssetData.AssetName.ToString();
				const FString RightName = Right.AssetData.AssetName.ToString();
				if (LeftName != RightName) return LeftName < RightName;
				return Left.AssetData.ObjectPath.LexicalLess(Right.AssetData.ObjectPath);
			});

			const FString ReportDirectory = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetAnalytics"));
			IFileManager::Get().MakeDirectory(*ReportDirectory, true);
			const FString CsvPath = FPaths::Combine(ReportDirectory, TEXT("MeshAnalytics.csv"));
			if (!FFileHelper::SaveStringToFile(
				BuildCsvData(),
				*CsvPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				ProgressNotification.Finish(
					LOCTEXT("AutomaticExportFailed", "The mesh analytics CSV file could not be saved."),
					SNotificationItem::CS_Fail);
				return;
			}

			const FString ReportUrl = Core::GetAssetAnalyticsReportUrl();
			FPlatformProcess::LaunchURL(*ReportUrl, nullptr, nullptr);
			ProgressNotification.Finish(
				FText::Format(
					LOCTEXT("MeshAnalyticsComplete", "Mesh Analytics report generated for {0} assets."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			PendingAssets.Reset();
		}

		/** Returns CSV text containing the enabled columns and finished rows. */
		FString BuildCsvData() const
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			TArray<FString> HeaderFields;
			HeaderFields.Add(TEXT("AssetName"));
			HeaderFields.Add(TEXT("PackagePath"));
			HeaderFields.Add(TEXT("IsSkeletalMesh"));
			if (Settings->bPackageDiskSize) HeaderFields.Add(TEXT("PackageDiskSize"));
			if (Settings->bLODCount) HeaderFields.Add(TEXT("LODCount"));
			if (Settings->bLODGroup) HeaderFields.Add(TEXT("LODGroup"));
			if (Settings->bCollisionInfo)
			{
				HeaderFields.Add(TEXT("CollisionComplexity"));
				HeaderFields.Add(TEXT("SimpleCollisionPrimitives"));
			}
			if (Settings->bPhysicsSize) HeaderFields.Add(TEXT("PhysicsSizeMB"));
			if (Settings->bComplexCollisionInfo) HeaderFields.Add(TEXT("ComplexCollisionVertices"));
			if (Settings->bTriangleCount) HeaderFields.Add(TEXT("LOD0Triangles"));
			if (Settings->bNaniteInfo)
			{
				HeaderFields.Add(TEXT("NaniteEnabled"));
				HeaderFields.Add(TEXT("NaniteTriangles"));
				HeaderFields.Add(TEXT("NaniteFallbackPercent"));
			}
			if (Settings->bMaterialCount) HeaderFields.Add(TEXT("MaterialSlots"));
			if (Settings->bTextureCount) HeaderFields.Add(TEXT("UniqueTextures"));
			if (Settings->bMaxTextureResolution) HeaderFields.Add(TEXT("MaxTextureResolution"));
			if (Settings->bUVChannelCount) HeaderFields.Add(TEXT("UVChannels"));
			if (Settings->bLightmapResolution) HeaderFields.Add(TEXT("LightmapResolution"));
			if (Settings->bMaxBoundsLength) HeaderFields.Add(TEXT("MaxBoundsLengthM"));

			FString CsvData = FString::Join(HeaderFields, TEXT(",")) + TEXT("\r\n");
			for (const FMeshAssetData& Item : AssetItems)
			{
				TArray<FString> RowFields;
				RowFields.Add(Core::EscapeCsvField(Item.AssetData.AssetName.ToString()));
				RowFields.Add(Core::EscapeCsvField(Item.AssetData.PackagePath.ToString()));
				RowFields.Add(Item.bIsSkeletalMesh ? TEXT("True") : TEXT("False"));
				if (Settings->bPackageDiskSize) RowFields.Add(Core::CsvNumberOrEmpty(Item.PackageDiskSize));
				if (Settings->bLODCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.LODCount));
				if (Settings->bLODGroup) RowFields.Add(Core::EscapeCsvField(Item.LODGroup));
				if (Settings->bCollisionInfo)
				{
					RowFields.Add(Core::EscapeCsvField(Item.CollisionComplexity));
					RowFields.Add(Core::CsvNumberOrEmpty(Item.SimpleCollisionPrimitives));
				}
				if (Settings->bPhysicsSize)
				{
					const double PhysicsSizeMB = Item.PhysicsSize < 0
						? -1.0
						: static_cast<double>(Item.PhysicsSize) / (1024.0 * 1024.0);
					RowFields.Add(Core::CsvNumberOrEmpty(PhysicsSizeMB));
				}
				if (Settings->bComplexCollisionInfo) RowFields.Add(Core::CsvNumberOrEmpty(Item.ComplexCollisionVertices));
				if (Settings->bTriangleCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.TriangleCount));
				if (Settings->bNaniteInfo)
				{
					RowFields.Add(Item.bNaniteEnabled ? TEXT("True") : TEXT("False"));
					RowFields.Add(Core::CsvNumberOrEmpty(Item.NaniteTriangles));
					RowFields.Add(Core::CsvNumberOrEmpty(Item.NaniteFallbackPercent));
				}
				if (Settings->bMaterialCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaterialSlotCount));
				if (Settings->bTextureCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.TextureCount));
				if (Settings->bMaxTextureResolution) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaxTextureResolution));
				if (Settings->bUVChannelCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.UVChannelCount));
				if (Settings->bLightmapResolution) RowFields.Add(Core::CsvNumberOrEmpty(Item.LightmapResolution));
				if (Settings->bMaxBoundsLength) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaxBoundsLengthM));
				CsvData += FString::Join(RowFields, TEXT(",")) + TEXT("\r\n");
			}
			return CsvData;
		}

		static constexpr double NotificationDisplayDelaySeconds = 0.2;
		/** Longest time one tick spends processing assets before letting the editor repaint. */
		static constexpr double TickTimeBudgetSeconds = 0.05;
		TArray<FMeshAssetData> AssetItems;
		TArray<FAssetData> PendingAssets;
		Core::FReportPackageUnloader PackageUnloader;
		FTSTicker::FDelegateHandle TickerHandle;
		/** Notification that shows the report progress and the Cancel button. */
		Core::FReportProgressNotification ProgressNotification;
		int32 NextAssetIndex = 0;
		/** Stage the next Tick runs. */
		ERunnerStep Step = ERunnerStep::ShowNotification;
		/** When the report started, used to delay blocking work until the notification is visible. */
		double StepStartSeconds = 0.0;
		/** Assets processed in the current batch. */
		int32 AssetsInBatch = 0;
		/** Whether the current batch loaded any asset from disk. */
		bool bBatchLoadedAnyAsset = false;
		/** Packages that were loaded before the current batch started. */
		TSet<UPackage*> PackagesBeforeBatch;
		/** Name of the last asset processed, shown in the notification. */
		FString CurrentAssetName;
	};

	static TUniquePtr<FMeshAnalyticsRunner> MeshAnalyticsRunner;

	/** Starts the shared Mesh Analytics report runner. */
	void RunMeshAnalyticsReport()
	{
		if (!MeshAnalyticsRunner) MeshAnalyticsRunner = MakeUnique<FMeshAnalyticsRunner>();
		MeshAnalyticsRunner->Start();
	}

	/** Stops the shared report runner and releases it. */
	void StopMeshAnalyticsReport()
	{
		if (MeshAnalyticsRunner)
		{
			MeshAnalyticsRunner->Stop();
			MeshAnalyticsRunner.Reset();
		}
	}
}
}

#undef LOCTEXT_NAMESPACE
