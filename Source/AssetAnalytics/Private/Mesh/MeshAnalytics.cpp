// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mesh/MeshAnalytics.h"
#include "Mesh/MeshAnalyticsSettings.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

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
		int32 LODCount = INDEX_NONE;
		FString LODGroup;
		FString CollisionComplexity;
		int32 SimpleCollisionPrimitives = INDEX_NONE;
		int32 ComplexCollisionVertices = INDEX_NONE;
		int32 VertexCount = INDEX_NONE;
		int32 MaterialSlotCount = INDEX_NONE;
		int32 TextureCount = INDEX_NONE;
		int32 MaxTextureResolution = INDEX_NONE;
		double AverageTextureResolution = -1.0;
		int32 UVChannelCount = INDEX_NONE;
		int32 LightmapResolution = INDEX_NONE;
		double MaxBoundsLengthM = -1.0;
	};

	/** Fills Item with the material and texture data enabled in Settings. */
	static void GatherMaterialAnalyticsData(
		const TArray<UMaterialInterface*>& Materials,
		const UMeshAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bMaterialCount) Item.MaterialSlotCount = Materials.Num();

		const bool bCollectTextureCount = Settings.bTextureCount;
		const bool bCollectTextureResolution = Settings.bMaxTextureResolution || Settings.bAverageTextureResolution;
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
		int64 TotalTextureResolution = 0;
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
			TotalTextureResolution += TextureResolution;
		}
		Item.MaxTextureResolution = MaxTextureResolution;
		if (Settings.bAverageTextureResolution)
		{
			Item.AverageTextureResolution = UniqueTextures.Num() > 0
				? static_cast<double>(TotalTextureResolution) / UniqueTextures.Num()
				: 0.0;
		}
	}

	/** Fills Item with the enabled skeletal mesh data. */
	static void GatherSkeletalMeshData(
		USkeletalMesh& SkeletalMesh,
		const UMeshAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		Item.bIsSkeletalMesh = true;
		if (Settings.bLODGroup) Item.LODGroup = TEXT("None");
		if (Settings.bLODCount) Item.LODCount = SkeletalMesh.GetLODNum();

		const bool bGatherMaterialData =
			Settings.bMaterialCount ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution ||
			Settings.bAverageTextureResolution;
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
		if (Settings.bVertexCount || Settings.bUVChannelCount)
		{
			const FSkeletalMeshRenderData* RenderData = SkeletalMesh.GetResourceForRendering();
			if (RenderData != nullptr && RenderData->LODRenderData.Num() > 0)
			{
				const FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[0];
				if (Settings.bVertexCount) Item.VertexCount = static_cast<int32>(LODRenderData.GetNumVertices());
				if (Settings.bUVChannelCount) Item.UVChannelCount = static_cast<int32>(LODRenderData.GetNumTexCoords());
			}
		}
	}

	/** Fills Item with the enabled static mesh data. */
	static void GatherStaticMeshData(
		UStaticMesh& StaticMesh,
		const UMeshAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bLODCount) Item.LODCount = StaticMesh.GetNumLODs();
		if (Settings.bLODGroup) Item.LODGroup = StaticMesh.GetLODGroup().ToString();
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
			Settings.bMaxTextureResolution ||
			Settings.bAverageTextureResolution;
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
		if ((Settings.bVertexCount || Settings.bUVChannelCount) &&
			StaticMesh.HasValidRenderData(true, 0))
		{
			if (Settings.bVertexCount) Item.VertexCount = StaticMesh.GetNumVertices(0);
			if (Settings.bUVChannelCount) Item.UVChannelCount = StaticMesh.GetNumUVChannels(0);
		}
	}

	/** Fills Item with values cached in the Asset Registry. */
	static void GatherAssetRegistryData(
		const FAssetData& AssetData,
		const UMeshAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		Item.bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		if (Settings.bLODCount) AssetData.GetTagValue(TEXT("LODs"), Item.LODCount);
		if (Settings.bVertexCount) AssetData.GetTagValue(TEXT("Vertices"), Item.VertexCount);

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
		const UMeshAnalyticsSettings& Settings)
	{
		const bool bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		if (Settings.bTextureCount || Settings.bMaxTextureResolution || Settings.bAverageTextureResolution) return true;
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
		const FAssetData& AssetData,
		UObject* MeshAsset,
		const UMeshAnalyticsSettings& Settings)
	{
		FMeshAssetData Item(AssetData);
		GatherAssetRegistryData(AssetData, Settings, Item);
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(MeshAsset)) GatherSkeletalMeshData(*SkeletalMesh, Settings, Item);
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(MeshAsset)) GatherStaticMeshData(*StaticMesh, Settings, Item);
		return Item;
	}

	/** Returns whether AssetData has a reference allowed by the current filter settings. */
	static bool HasRelevantReferencer(
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

	class FMeshAnalyticsRunner
	{
	public:
		/** Starts gathering unless a report is already running. */
		void Start()
		{
			if (TickerHandle.IsValid()) return;
			ShowProgressNotification();
			BeginGatheringAssets();
			UpdateProgressNotification();
			if (PendingAssets.Num() == 0)
			{
				CompleteReport();
				return;
			}
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
			PendingAssets.Reset();
			AssetItems.Reset();
			NextAssetIndex = 0;
			UnloadReportPackages();
			DismissProgressNotification();
		}

	private:
		/** Shows the progress notification used while assets are gathered. */
		void ShowProgressNotification()
		{
			FNotificationInfo NotificationInfo(LOCTEXT("FindingMeshAssets", "Finding mesh assets..."));
			NotificationInfo.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("CancelMeshAnalytics", "Cancel"),
				LOCTEXT("CancelMeshAnalyticsTooltip", "Stop gathering mesh analytics."),
				FSimpleDelegate::CreateRaw(this, &FMeshAnalyticsRunner::Cancel)));
			NotificationInfo.WidthOverride = 320.0f;
			NotificationInfo.bFireAndForget = false;
			NotificationInfo.FadeOutDuration = 0.5f;
			NotificationInfo.ExpireDuration = 3.0f;
			NotificationInfo.bUseLargeFont = false;
			NotificationInfo.bUseThrobber = false;
			NotificationInfo.bUseSuccessFailIcons = false;
			ProgressNotificationItem = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
			if (ProgressNotificationItem.IsValid()) ProgressNotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
		}

		/** Updates the notification with the current asset count. */
		void UpdateProgressNotification() const
		{
			if (ProgressNotificationItem.IsValid())
			{
				const int32 ProgressPercent = PendingAssets.Num() > 0
					? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
					: 100;
				ProgressNotificationItem->SetText(FText::Format(
					LOCTEXT("MeshAnalyticsProgress", "Gathering Mesh Analytics - {0} / {1} assets ({2}%)"),
					FText::AsNumber(NextAssetIndex),
					FText::AsNumber(PendingAssets.Num()),
					FText::AsNumber(ProgressPercent)));
			}
		}

		/** Shows ResultText and State, then closes the progress notification. */
		void FinishProgressNotification(
			const FText& ResultText,
			const SNotificationItem::ECompletionState State)
		{
			if (ProgressNotificationItem.IsValid())
			{
				ProgressNotificationItem->SetText(ResultText);
				ProgressNotificationItem->SetCompletionState(State);
				ProgressNotificationItem->ExpireAndFadeout();
			}
			ProgressNotificationItem.Reset();
		}

		/** Hides the progress notification during shutdown. */
		void DismissProgressNotification()
		{
			if (ProgressNotificationItem.IsValid())
			{
				ProgressNotificationItem->SetCompletionState(SNotificationItem::CS_None);
				ProgressNotificationItem->Fadeout();
			}
			ProgressNotificationItem.Reset();
		}

		/** Safely unloads mesh packages that were loaded only for this report. */
		void UnloadReportPackages()
		{
			if (PackagesLoadedForReport.Num() == 0) return;

			FText UnloadError;
			UPackageTools::UnloadPackages(PackagesLoadedForReport, UnloadError);
			if (!UnloadError.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not unload some report packages: %s"), *UnloadError.ToString());
			}
			PackagesLoadedForReport.Reset();
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			PendingAssets.Reset();
			AssetItems.Reset();
			NextAssetIndex = 0;
			UnloadReportPackages();
			FinishProgressNotification(
				LOCTEXT("MeshAnalyticsCancelled", "Mesh Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/** Finds the mesh assets in the configured folders. */
		void BeginGatheringAssets()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			NextAssetIndex = 0;
			PackagesLoadedForReport.Reset();

			const UMeshAnalyticsSettings* Settings = GetDefault<UMeshAnalyticsSettings>();
			FARFilter Filter;
			if (Settings->bStaticMeshes) Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
			if (Settings->bSkeletalMeshes) Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			for (const FDirectoryPath& Directory : Settings->SearchFolders)
			{
				FName PackagePath;
				if (Core::TryGetPackagePath(Directory.Path, PackagePath)) Filter.PackagePaths.AddUnique(PackagePath);
			}
			if (Filter.ClassPaths.Num() == 0 || Filter.PackagePaths.Num() == 0) return;

			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);
		}

		/**
		 * Processes the next batch and loads assets only for columns that require it.
		 * Returns: True while more assets are waiting.
		 */
		bool Tick(float)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			const UMeshAnalyticsSettings* Settings = GetDefault<UMeshAnalyticsSettings>();
			const int32 BatchEnd = FMath::Min(NextAssetIndex + AssetsPerTick, PendingAssets.Num());

			for (; NextAssetIndex < BatchEnd; ++NextAssetIndex)
			{
				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				if (Settings->bIgnoreUnreferencedAssets &&
					!HasRelevantReferencer(AssetRegistry, AssetData, Settings->bIgnoreDeveloperReferences)) continue;
				UObject* MeshAsset = nullptr;
				bool bWasPackageLoaded = false;
				if (NeedsAssetLoading(AssetData, *Settings))
				{
					bWasPackageLoaded = FindPackage(nullptr, *AssetData.PackageName.ToString()) != nullptr;
					MeshAsset = AssetData.GetAsset();
				}
				AssetItems.Add(MakeMeshAssetData(AssetData, MeshAsset, *Settings));
				if (!bWasPackageLoaded && MeshAsset != nullptr) PackagesLoadedForReport.AddUnique(MeshAsset->GetOutermost());
			}
			UnloadReportPackages();
			UpdateProgressNotification();

			if (NextAssetIndex >= PendingAssets.Num())
			{
				TickerHandle.Reset();
				CompleteReport();
				return false;
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
				FinishProgressNotification(
					LOCTEXT("AutomaticExportFailed", "The mesh analytics CSV file could not be saved."),
					SNotificationItem::CS_Fail);
				return;
			}

			const FString ReportUrl = Core::GetAssetAnalyticsReportUrl();
			FPlatformProcess::LaunchURL(*ReportUrl, nullptr, nullptr);
			FinishProgressNotification(
				FText::Format(
					LOCTEXT("MeshAnalyticsComplete", "Mesh Analytics report generated for {0} assets."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			PendingAssets.Reset();
		}

		/** Returns CSV text containing the enabled columns and finished rows. */
		FString BuildCsvData() const
		{
			const UMeshAnalyticsSettings* Settings = GetDefault<UMeshAnalyticsSettings>();
			TArray<FString> HeaderFields;
			HeaderFields.Add(TEXT("AssetName"));
			HeaderFields.Add(TEXT("PackagePath"));
			HeaderFields.Add(TEXT("IsSkeletalMesh"));
			if (Settings->bLODCount) HeaderFields.Add(TEXT("LODCount"));
			if (Settings->bLODGroup) HeaderFields.Add(TEXT("LODGroup"));
			if (Settings->bCollisionInfo)
			{
				HeaderFields.Add(TEXT("CollisionComplexity"));
				HeaderFields.Add(TEXT("SimpleCollisionPrimitives"));
			}
			if (Settings->bComplexCollisionInfo) HeaderFields.Add(TEXT("ComplexCollisionVertices"));
			if (Settings->bVertexCount) HeaderFields.Add(TEXT("LOD0Vertices"));
			if (Settings->bMaterialCount) HeaderFields.Add(TEXT("MaterialSlots"));
			if (Settings->bTextureCount) HeaderFields.Add(TEXT("UniqueTextures"));
			if (Settings->bMaxTextureResolution) HeaderFields.Add(TEXT("MaxTextureResolution"));
			if (Settings->bAverageTextureResolution) HeaderFields.Add(TEXT("AverageTextureResolution"));
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
				if (Settings->bLODCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.LODCount));
				if (Settings->bLODGroup) RowFields.Add(Core::EscapeCsvField(Item.LODGroup));
				if (Settings->bCollisionInfo)
				{
					RowFields.Add(Core::EscapeCsvField(Item.CollisionComplexity));
					RowFields.Add(Core::CsvNumberOrEmpty(Item.SimpleCollisionPrimitives));
				}
				if (Settings->bComplexCollisionInfo) RowFields.Add(Core::CsvNumberOrEmpty(Item.ComplexCollisionVertices));
				if (Settings->bVertexCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.VertexCount));
				if (Settings->bMaterialCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaterialSlotCount));
				if (Settings->bTextureCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.TextureCount));
				if (Settings->bMaxTextureResolution) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaxTextureResolution));
				if (Settings->bAverageTextureResolution) RowFields.Add(Core::CsvNumberOrEmpty(Item.AverageTextureResolution));
				if (Settings->bUVChannelCount) RowFields.Add(Core::CsvNumberOrEmpty(Item.UVChannelCount));
				if (Settings->bLightmapResolution) RowFields.Add(Core::CsvNumberOrEmpty(Item.LightmapResolution));
				if (Settings->bMaxBoundsLength) RowFields.Add(Core::CsvNumberOrEmpty(Item.MaxBoundsLengthM));
				CsvData += FString::Join(RowFields, TEXT(",")) + TEXT("\r\n");
			}
			return CsvData;
		}

		static constexpr int32 AssetsPerTick = 10;
		TArray<FMeshAssetData> AssetItems;
		TArray<FAssetData> PendingAssets;
		TArray<UPackage*> PackagesLoadedForReport;
		FTSTicker::FDelegateHandle TickerHandle;
		TSharedPtr<SNotificationItem> ProgressNotificationItem;
		int32 NextAssetIndex = 0;
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
