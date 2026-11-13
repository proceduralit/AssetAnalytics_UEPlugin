// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Material/MaterialAnalytics.h"
#include "Core/AssetAnalyticsPackageUnloader.h"
#include "Core/AssetAnalyticsProgressNotification.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Core/AssetAnalyticsSettings.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/MeshDeformer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Particles/Material/ParticleModuleMeshMaterial.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/TypeData/ParticleModuleTypeDataAnimTrail.h"
#include "Particles/TypeData/ParticleModuleTypeDataBeam2.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Particles/TypeData/ParticleModuleTypeDataRibbon.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialCachedData.h"
#include "MaterialShaderQualitySettings.h"
#include "MaterialShared.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RHIShaderPlatform.h"
#include "SceneTypes.h"
#include "ShaderPlatformQualitySettings.h"
#include "StaticParameterSet.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "UObject/LinkerLoad.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "MaterialAnalytics"

namespace AssetAnalytics
{
namespace Material
{
	/** Kinds of material users that can be recognised from Asset Registry data alone. */
	enum class EMaterialUserType : uint8
	{
		StaticMesh,
		Nanite,
		SkeletalMesh,
		InstancedStaticMeshes,
		Niagara,
		Particle,
		GeometryCollections,
		GeometryCache,
		HairStrands,
		LidarPointCloud,
		Water,
		VolumetricCloud,
		HeterogeneousVolumes,
		VirtualHeightfieldMesh,
		Level,
		Blueprint,
		Other,
		/** Also used in the vertex factory table for entries whose users cannot be found without loading. */
		Count
	};

	/** How the users of a vertex factory entry are found when the Asset Registry cannot list them. */
	enum class EUnregisteredUserLookup : uint8
	{
		/** Loading Blueprint, Cascade particle system and skeletal mesh users can find them. */
		Loading,
		/** Users are actors placed in levels, which the report does not check. */
		PlacedActors,
		/** Users are Niagara systems, which the report does not load. */
		Niagara,
		/** The report has no way to find these users. */
		NotChecked
	};

	/** One entry of the VertexFactories object in the JSON report. */
	struct FVertexFactoryEntry
	{
		/** JSON key: the engine's EMaterialUsage name without MATUSAGE_, or a user type that has no usage flag. */
		const TCHAR* Name;

		/** Registry user type listed under this entry, or Count when the registry cannot find its users. */
		EMaterialUserType RegistryUserType;

		/** Engine usage flag this entry reports as Enabled, or MATUSAGE_MAX for entries that are not usage flags. */
		EMaterialUsage Usage;

		/** How users are found when RegistryUserType is Count. Unused for registry entries. */
		EUnregisteredUserLookup UnregisteredUserLookup = EUnregisteredUserLookup::NotChecked;
	};

	/**
	 * Every entry written under VertexFactories: all EMaterialUsage flags in engine order,
	 * then the user types that do not match a single usage flag.
	 */
	static const FVertexFactoryEntry VertexFactoryEntries[] =
	{
		{ TEXT("SkeletalMesh"), EMaterialUserType::SkeletalMesh, MATUSAGE_SkeletalMesh },
		{ TEXT("ParticleSprites"), EMaterialUserType::Count, MATUSAGE_ParticleSprites, EUnregisteredUserLookup::Loading },
		{ TEXT("BeamTrails"), EMaterialUserType::Count, MATUSAGE_BeamTrails, EUnregisteredUserLookup::Loading },
		{ TEXT("MeshParticles"), EMaterialUserType::Count, MATUSAGE_MeshParticles, EUnregisteredUserLookup::Loading },
		{ TEXT("StaticLighting"), EMaterialUserType::Count, MATUSAGE_StaticLighting, EUnregisteredUserLookup::PlacedActors },
		{ TEXT("MorphTargets"), EMaterialUserType::Count, MATUSAGE_MorphTargets, EUnregisteredUserLookup::Loading },
		{ TEXT("SplineMesh"), EMaterialUserType::Count, MATUSAGE_SplineMesh, EUnregisteredUserLookup::Loading },
		{ TEXT("InstancedStaticMeshes"), EMaterialUserType::InstancedStaticMeshes, MATUSAGE_InstancedStaticMeshes },
		{ TEXT("GeometryCollections"), EMaterialUserType::GeometryCollections, MATUSAGE_GeometryCollections },
		{ TEXT("Clothing"), EMaterialUserType::Count, MATUSAGE_Clothing, EUnregisteredUserLookup::Loading },
		{ TEXT("NiagaraSprites"), EMaterialUserType::Count, MATUSAGE_NiagaraSprites, EUnregisteredUserLookup::Niagara },
		{ TEXT("NiagaraRibbons"), EMaterialUserType::Count, MATUSAGE_NiagaraRibbons, EUnregisteredUserLookup::Niagara },
		{ TEXT("NiagaraMeshParticles"), EMaterialUserType::Count, MATUSAGE_NiagaraMeshParticles, EUnregisteredUserLookup::Niagara },
		{ TEXT("GeometryCache"), EMaterialUserType::GeometryCache, MATUSAGE_GeometryCache },
		{ TEXT("Water"), EMaterialUserType::Water, MATUSAGE_Water },
		{ TEXT("HairStrands"), EMaterialUserType::HairStrands, MATUSAGE_HairStrands },
		{ TEXT("LidarPointCloud"), EMaterialUserType::LidarPointCloud, MATUSAGE_LidarPointCloud },
		{ TEXT("VirtualHeightfieldMesh"), EMaterialUserType::VirtualHeightfieldMesh, MATUSAGE_VirtualHeightfieldMesh },
		{ TEXT("Nanite"), EMaterialUserType::Nanite, MATUSAGE_Nanite },
		{ TEXT("Voxels"), EMaterialUserType::Count, MATUSAGE_Voxels },
		{ TEXT("VolumetricCloud"), EMaterialUserType::VolumetricCloud, MATUSAGE_VolumetricCloud },
		{ TEXT("HeterogeneousVolumes"), EMaterialUserType::HeterogeneousVolumes, MATUSAGE_HeterogeneousVolumes },
		{ TEXT("StaticMesh"), EMaterialUserType::StaticMesh, MATUSAGE_StaticMesh },
		{ TEXT("EditorCompositing"), EMaterialUserType::Count, MATUSAGE_EditorCompositing },
		{ TEXT("NeuralNetworks"), EMaterialUserType::Count, MATUSAGE_NeuralNetworks },
		{ TEXT("MeshDeformer"), EMaterialUserType::Count, MATUSAGE_MeshDeformer, EUnregisteredUserLookup::Loading },
		{ TEXT("Niagara"), EMaterialUserType::Niagara, MATUSAGE_MAX },
		{ TEXT("Particle"), EMaterialUserType::Particle, MATUSAGE_MAX },
		{ TEXT("Level"), EMaterialUserType::Level, MATUSAGE_MAX },
		{ TEXT("Blueprint"), EMaterialUserType::Blueprint, MATUSAGE_MAX },
		{ TEXT("Other"), EMaterialUserType::Other, MATUSAGE_MAX },
	};

	/** A classified asset that references a material or one of its instances. */
	struct FMaterialUser
	{
		/** Kind of asset or actor that uses the material. */
		EMaterialUserType Type = EMaterialUserType::Other;

		/** Object path shown in the report, or the package name when the package has no registry asset. */
		FString Path;
	};

	/** Kinds of instance overrides that give an instance its own shader map. */
	enum class EPermutationParameterKind : uint8
	{
		StaticSwitch,
		ComponentMask,
		BaseProperty
	};

	/** One static parameter or base property of a master and how its counted instances override it. */
	struct FPermutationParameterSummary
	{
		/** Whether this is a static switch, a static component mask or a base property such as blend mode. */
		EPermutationParameterKind Kind = EPermutationParameterKind::StaticSwitch;

		/** Identifies the parameter, including its layer when it comes from a material layer. Base properties only set Name. */
		FMaterialParameterInfo ParameterInfo;

		/** Value the master uses when an instance does not override it, as report text. */
		FString DefaultValue;

		/** Counted instances whose own override gives a value different from their parent's. */
		int32 ChangedCount = 0;

		/** Counted instances whose own override repeats the value they would inherit from their parent. */
		int32 NoOpCount = 0;
	};

	/** A static parameter or base property of one instance that the instance overrides or that differs from the master. */
	struct FInstancePermutationCell
	{
		/** Index of the parameter in FMaterialAssetData::PermutationParameters. */
		int32 ParameterIndex = INDEX_NONE;

		/** Value the instance ends up with, as report text. */
		FString Value;

		/** Whether the instance's own overrides set this parameter, rather than inheriting it from its parent. */
		bool bOwnOverride = false;

		/** Whether the instance's own override repeats the value it would inherit from its parent. */
		bool bSameAsParent = false;
	};

	/** Static parameters of one counted instance. */
	struct FInstancePermutationRow
	{
		/** Object path of the instance. */
		FString Path;

		/** Parameters the instance overrides or ends up with a value different from the master's. */
		TArray<FInstancePermutationCell> Cells;
	};

	/** Everything the report gathers for one master material. */
	struct FMaterialAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMaterialAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		/** Registry entry of the master material. */
		FAssetData AssetData;

		/** Size of the master's package on disk in bytes, or -1 when unknown. */
		int64 PackageDiskSize = -1;

		/** Counted instances below the material, including instances of instances. */
		TArray<FAssetData> Instances;

		/** User paths for each EMaterialUserType, sorted. Empty when users were not gathered. */
		TArray<TArray<FString>> UserPathsByType;

		/** Whether each EMaterialUsage flag is enabled on the master. Empty when the master was not read. */
		TArray<bool> UsageEnabled;

		/** Loaded users found for each EMaterialUsage flag, sorted. Empty when users were not loaded. */
		TArray<TArray<FString>> ResolvedUserPathsByUsage;

		/** Whether the counted instances were loaded to find which ones compile their own shader map. */
		bool bShaderPermutationsRead = false;

		/** Counted instances that compile their own shader map, sorted. */
		TArray<FString> StaticPermutationInstancePaths;

		/** Shaders the master and one instance per unique static permutation compile, from the engine's shader map layout. */
		int32 LayoutShaderCount = INDEX_NONE;

		/** Static switches, component masks and base properties of the master, with how counted instances override them. */
		TArray<FPermutationParameterSummary> PermutationParameters;

		/** Static parameters of every loaded counted instance, sorted by path. */
		TArray<FInstancePermutationRow> InstancePermutationRows;
	};

	/**
	 * Effective static parameters and base property overrides of a material, used to group materials that compile
	 * the same shader map. Matches what the engine puts in a material's shader map ID.
	 */
	struct FStaticPermutationKey
	{
		/** Static parameters the material ends up with, including inherited ones, with every bOverride cleared. */
		FStaticParameterSet StaticParameters;

		/** Hash of the base properties that differ from the master's, such as blend mode or two sided; 0 when none do. */
		uint64 BasePropertyOverridesHash = 0;

		/** Returns whether Other compiles the same shader map as this key. */
		bool CompilesSameShaders(const FStaticPermutationKey& Other) const
		{
			return BasePropertyOverridesHash == Other.BasePropertyOverridesHash &&
				StaticParameters.Equivalent(Other.StaticParameters);
		}
	};

	/** A base property an instance can override, read through UMaterialInterface so inherited values are included. */
	struct FBasePropertyCheck
	{
		/** Property name shown in the report. */
		const TCHAR* Name;

		/** Returns whether the instance's own overrides set this property. */
		bool (*IsOverridden)(const FMaterialInstanceBasePropertyOverrides& Overrides);

		/** Returns whether the two materials end up with the same value for this property. */
		bool (*HasSameValue)(const UMaterialInterface& Left, const UMaterialInterface& Right);

		/** Returns the material's value for this property as report text, such as "Masked" or "True". */
		FString (*ValueText)(const UMaterialInterface& Material);
	};

	/** Compares two base property values the way UMaterialInstance::GetBasePropertyOverridesHash does. */
	template <typename ValueType>
	static bool HaveSameBasePropertyValue(const ValueType& Left, const ValueType& Right)
	{
		if constexpr (std::is_floating_point_v<ValueType>) return FMath::IsNearlyEqual(Left, Right);
		else return !(Left != Right);
	}

	/** Formats a base property value as report text. One overload per value type the base property getters return. */
	static FString FormatBasePropertyValue(const bool bValue) { return bValue ? TEXT("True") : TEXT("False"); }
	static FString FormatBasePropertyValue(const float Value) { return FString::SanitizeFloat(Value, 0); }
	static FString FormatBasePropertyValue(const uint32 Flags) { return FString::Printf(TEXT("0x%X"), Flags); }

	static FString FormatBasePropertyValue(const EBlendMode BlendMode)
	{
		return StaticEnum<EBlendMode>()->GetDisplayNameTextByValue(BlendMode).ToString();
	}

	/** Lists every shading model in the field, such as "Default Lit, Subsurface". */
	static FString FormatBasePropertyValue(const FMaterialShadingModelField ShadingModels)
	{
		TArray<FString> ShadingModelNames;
		for (int32 ShadingModel = 0; ShadingModel < MSM_NUM; ++ShadingModel)
		{
			if (!ShadingModels.HasShadingModel(static_cast<EMaterialShadingModel>(ShadingModel))) continue;
			ShadingModelNames.Add(StaticEnum<EMaterialShadingModel>()->GetDisplayNameTextByValue(ShadingModel).ToString());
		}
		return FString::Join(ShadingModelNames, TEXT(", "));
	}

	static FString FormatBasePropertyValue(const FDisplacementScaling& Scaling)
	{
		return FString::Printf(
			TEXT("Magnitude %s, Center %s"),
			*FString::SanitizeFloat(Scaling.Magnitude, 0),
			*FString::SanitizeFloat(Scaling.Center, 0));
	}

	static FString FormatBasePropertyValue(const FDisplacementFadeRange& FadeRange)
	{
		return FString::Printf(
			TEXT("Start %s px, End %s px"),
			*FString::SanitizeFloat(FadeRange.StartSizePixels, 0),
			*FString::SanitizeFloat(FadeRange.EndSizePixels, 0));
	}

#define ASSET_ANALYTICS_BASE_PROPERTY(Name, OverrideFlag, Getter) \
	{ \
		TEXT(Name), \
		[](const FMaterialInstanceBasePropertyOverrides& Overrides) { return Overrides.OverrideFlag != 0; }, \
		[](const UMaterialInterface& Left, const UMaterialInterface& Right) { return HaveSameBasePropertyValue(Left.Getter(), Right.Getter()); }, \
		[](const UMaterialInterface& Material) { return FormatBasePropertyValue(Material.Getter()); } \
	}

	/** The base properties UMaterialInstance::GetBasePropertyOverridesHash puts in a shader map ID, in the same order. */
	static const FBasePropertyCheck BasePropertyChecks[] =
	{
		ASSET_ANALYTICS_BASE_PROPERTY("OpacityMaskClipValue", bOverride_OpacityMaskClipValue, GetOpacityMaskClipValue),
		ASSET_ANALYTICS_BASE_PROPERTY("BlendMode", bOverride_BlendMode, GetBlendMode),
		ASSET_ANALYTICS_BASE_PROPERTY("ShadingModel", bOverride_ShadingModel, GetShadingModels),
		ASSET_ANALYTICS_BASE_PROPERTY("TwoSided", bOverride_TwoSided, IsTwoSided),
		ASSET_ANALYTICS_BASE_PROPERTY("ThinSurface", bOverride_bIsThinSurface, IsThinSurface),
		ASSET_ANALYTICS_BASE_PROPERTY("DitheredLODTransition", bOverride_DitheredLODTransition, IsDitheredLODTransition),
		ASSET_ANALYTICS_BASE_PROPERTY("CastDynamicShadowAsMasked", bOverride_CastDynamicShadowAsMasked, GetCastDynamicShadowAsMasked),
		ASSET_ANALYTICS_BASE_PROPERTY("OutputTranslucentVelocity", bOverride_OutputTranslucentVelocity, IsTranslucencyWritingVelocity),
		ASSET_ANALYTICS_BASE_PROPERTY("HasPixelAnimation", bOverride_bHasPixelAnimation, HasPixelAnimation),
		ASSET_ANALYTICS_BASE_PROPERTY("EnableTessellation", bOverride_bEnableTessellation, IsTessellationEnabled),
		ASSET_ANALYTICS_BASE_PROPERTY("DisplacementScaling", bOverride_DisplacementScaling, GetDisplacementScaling),
		ASSET_ANALYTICS_BASE_PROPERTY("EnableDisplacementFade", bOverride_bEnableDisplacementFade, IsDisplacementFadeEnabled),
		ASSET_ANALYTICS_BASE_PROPERTY("DisplacementFadeRange", bOverride_DisplacementFadeRange, GetDisplacementFadeRange),
		ASSET_ANALYTICS_BASE_PROPERTY("MaxWorldPositionOffsetDisplacement", bOverride_MaxWorldPositionOffsetDisplacement, GetMaxWorldPositionOffsetDisplacement),
		ASSET_ANALYTICS_BASE_PROPERTY("CompatibleWithLumenCardSharing", bOverride_CompatibleWithLumenCardSharing, IsCompatibleWithLumenCardSharing),
		ASSET_ANALYTICS_BASE_PROPERTY("UsageFlags", bOverride_UsageFlags, GetUsageFlags),
	};

#undef ASSET_ANALYTICS_BASE_PROPERTY

	/**
	 * Counts the shader maps Material compiles across quality levels on ShaderPlatform when cooking: one per quality
	 * level whose Quality Switch inputs differ or that a platform quality override forces, plus one default.
	 * It repeats UMaterialInterface::GetQualityLevelUsage (not exported from the Engine module) and
	 * UMaterial::GetNewResources: each used quality level gets its own shader map, and the unused levels share a
	 * default one unless the platform discards them when cooking.
	 */
	static int32 CountQualityShaderMaps(const UMaterialInterface& Material, const EShaderPlatform ShaderPlatform)
	{
		// Quality levels whose Quality Switch inputs differ, including switches inside material functions.
		const TArray<bool>& SwitchQualityLevelsUsed = Material.GetCachedExpressionData().QualityLevelsUsed;
		const UShaderPlatformQualitySettings* QualitySettings =
			UMaterialShaderQualitySettings::Get()->GetShaderPlatformQualitySettings(ShaderPlatform);

		int32 ShaderMapCount = 0;
		bool bNeedsDefaultShaderMap = false;
		for (int32 QualityIndex = 0; QualityIndex < EMaterialQualityLevel::Num; ++QualityIndex)
		{
			const EMaterialQualityLevel::Type QualityLevel = static_cast<EMaterialQualityLevel::Type>(QualityIndex);
			bool bQualityLevelUsed = SwitchQualityLevelsUsed.IsValidIndex(QualityIndex) && SwitchQualityLevelsUsed[QualityIndex];
			bool bDiscardedWhenCooking = false;
			if (QualitySettings != nullptr)
			{
				const FMaterialQualityOverrides& QualityOverrides = QualitySettings->GetQualityOverrides(QualityLevel);
				if (QualityOverrides.bDiscardQualityDuringCook)
				{
					bQualityLevelUsed = false;
					bDiscardedWhenCooking = true;
				}
				else if (QualityOverrides.bEnableOverride &&
					QualityOverrides.HasAnyOverridesSet() &&
					QualityOverrides.CanOverride(ShaderPlatform))
				{
					// Platform quality overrides change the shader, so this level compiles separately too.
					bQualityLevelUsed = true;
				}
			}

			if (bQualityLevelUsed)
			{
				++ShaderMapCount;
			}
			else if (!bDiscardedWhenCooking)
			{
				bNeedsDefaultShaderMap = true;
			}
		}
		if (bNeedsDefaultShaderMap) ++ShaderMapCount;
		return ShaderMapCount;
	}

	/**
	 * Counts the shaders Material's shader maps compile on ShaderPlatform, from the engine's shader map layout.
	 * Nothing is compiled: the layout lists every shader type each vertex factory would cache, using conservative
	 * material parameters, so the count can be slightly higher than a cook. This is the count the material editor's
	 * shader stats and the DumpMaterialShaderTypes commandlet use.
	 * A master covers all its quality shader maps; an instance only covers its active quality level.
	 * Inputs: CookedPlatform selects the cooked game's shader permutations (nullptr uses the editor's, which adds
	 * editor-only shaders). Multiplier repeats the count, for instance quality shader maps the layout does not cover.
	 * Returns: The shader count.
	 */
	static int32 CountLayoutShaders(
		UMaterialInterface& Material,
		const EShaderPlatform ShaderPlatform,
		const ITargetPlatform* CookedPlatform,
		const int32 Multiplier)
	{
		TArray<FDebugShaderTypeInfo> ShaderInfos;
		Material.GetShaderTypes(ShaderPlatform, CookedPlatform, ShaderInfos);

		int32 ShaderCount = 0;
		for (const FDebugShaderTypeInfo& ShaderInfo : ShaderInfos)
		{
			ShaderCount += ShaderInfo.ShaderTypes.Num();
			for (const FDebugShaderPipelineInfo& PipelineInfo : ShaderInfo.Pipelines)
			{
				ShaderCount += PipelineInfo.ShaderTypes.Num();
			}
		}
		return ShaderCount * Multiplier;
	}

	/**
	 * Decides which kind of placed World Partition actor uses a material, from the actor's class.
	 * Input: ActorAsset is the registry entry of an actor stored under __ExternalActors__.
	 * Returns: A specific actor-based user type, or Level for any other actor.
	 */
	static EMaterialUserType ClassifyPlacedActor(IAssetRegistry& AssetRegistry, const FAssetData& ActorAsset)
	{
		// Plugin classes are compared by path, so their modules are not needed as dependencies.
		static const FTopLevelAssetPath ISMPartitionActorClass(TEXT("/Script/Engine"), TEXT("ISMPartitionActor"));
		static const FTopLevelAssetPath WaterBodyClass(TEXT("/Script/Water"), TEXT("WaterBody"));
		static const FTopLevelAssetPath WaterZoneClass(TEXT("/Script/Water"), TEXT("WaterZone"));
		static const FTopLevelAssetPath VolumetricCloudClass(TEXT("/Script/Engine"), TEXT("VolumetricCloud"));
		static const FTopLevelAssetPath HeterogeneousVolumeClass(TEXT("/Script/Engine"), TEXT("HeterogeneousVolume"));
		static const FTopLevelAssetPath VirtualHeightfieldMeshClass(TEXT("/Script/VirtualHeightfieldMesh"), TEXT("VirtualHeightfieldMesh"));

		// Check the actor class and its parents, so subclasses such as WaterBodyRiver or InstancedFoliageActor match too.
		TArray<FTopLevelAssetPath> ActorClassAndAncestors;
		AssetRegistry.GetAncestorClassNames(ActorAsset.AssetClassPath, ActorClassAndAncestors);
		ActorClassAndAncestors.Add(ActorAsset.AssetClassPath);
		for (const FTopLevelAssetPath& ClassPath : ActorClassAndAncestors)
		{
			if (ClassPath == ISMPartitionActorClass) return EMaterialUserType::InstancedStaticMeshes;
			if (ClassPath == WaterBodyClass || ClassPath == WaterZoneClass) return EMaterialUserType::Water;
			if (ClassPath == VolumetricCloudClass) return EMaterialUserType::VolumetricCloud;
			if (ClassPath == HeterogeneousVolumeClass) return EMaterialUserType::HeterogeneousVolumes;
			if (ClassPath == VirtualHeightfieldMeshClass) return EMaterialUserType::VirtualHeightfieldMesh;
		}
		return EMaterialUserType::Level;
	}

	/**
	 * Decides which kind of asset a referencing package holds, using only registry data.
	 * Inputs: ReferencerPackage is a package that references a material or material instance, and PackageAssets
	 * are the registry assets in that package.
	 * Returns: The matching user type, or Other when no rule matches.
	 */
	static EMaterialUserType ClassifyReferencer(
		IAssetRegistry& AssetRegistry,
		const FName ReferencerPackage,
		const TArray<FAssetData>& PackageAssets)
	{
		// World Partition stores each placed actor in its own package under __ExternalActors__.
		const FString PackageName = ReferencerPackage.ToString();
		if (PackageName.Contains(FPackagePath::GetExternalActorsFolderName()))
		{
			return PackageAssets.Num() > 0
				? ClassifyPlacedActor(AssetRegistry, PackageAssets[0])
				: EMaterialUserType::Level;
		}
		if (PackageAssets.Num() == 0) return EMaterialUserType::Other;

		// Plugin classes are compared by path, so their modules are not needed as dependencies.
		static const FTopLevelAssetPath StaticMeshClass(TEXT("/Script/Engine"), TEXT("StaticMesh"));
		static const FTopLevelAssetPath SkeletalMeshClass(TEXT("/Script/Engine"), TEXT("SkeletalMesh"));
		static const FTopLevelAssetPath NiagaraSystemClass(TEXT("/Script/Niagara"), TEXT("NiagaraSystem"));
		static const FTopLevelAssetPath ParticleSystemClass(TEXT("/Script/Engine"), TEXT("ParticleSystem"));
		static const FTopLevelAssetPath GeometryCollectionClass(TEXT("/Script/GeometryCollectionEngine"), TEXT("GeometryCollection"));
		static const FTopLevelAssetPath GeometryCacheClass(TEXT("/Script/GeometryCache"), TEXT("GeometryCache"));
		static const FTopLevelAssetPath GroomClass(TEXT("/Script/HairStrandsCore"), TEXT("GroomAsset"));
		static const FTopLevelAssetPath LidarPointCloudClass(TEXT("/Script/LidarPointCloudRuntime"), TEXT("LidarPointCloud"));
		static const FTopLevelAssetPath WorldClass(TEXT("/Script/Engine"), TEXT("World"));

		const FAssetData& ReferencerAsset = PackageAssets[0];
		const FTopLevelAssetPath& ClassPath = ReferencerAsset.AssetClassPath;
		if (ClassPath == StaticMeshClass)
		{
			// The NaniteEnabled tag is saved with every static mesh, so Nanite meshes are told apart without loading.
			bool bNaniteEnabled = false;
			ReferencerAsset.GetTagValue(TEXT("NaniteEnabled"), bNaniteEnabled);
			return bNaniteEnabled ? EMaterialUserType::Nanite : EMaterialUserType::StaticMesh;
		}
		if (ClassPath == SkeletalMeshClass) return EMaterialUserType::SkeletalMesh;
		if (ClassPath == NiagaraSystemClass) return EMaterialUserType::Niagara;
		if (ClassPath == ParticleSystemClass) return EMaterialUserType::Particle;
		if (ClassPath == GeometryCollectionClass) return EMaterialUserType::GeometryCollections;
		if (ClassPath == GeometryCacheClass) return EMaterialUserType::GeometryCache;
		if (ClassPath == GroomClass) return EMaterialUserType::HairStrands;
		if (ClassPath == LidarPointCloudClass) return EMaterialUserType::LidarPointCloud;
		if (ClassPath == WorldClass) return EMaterialUserType::Level;

		// GetClass only finds classes that are already loaded, so no asset is loaded here.
		const UClass* AssetClass = ReferencerAsset.GetClass();
		if (AssetClass != nullptr && AssetClass->IsChildOf(UBlueprint::StaticClass())) return EMaterialUserType::Blueprint;
		return EMaterialUserType::Other;
	}

	/**
	 * Classifies a referencing package and picks the path shown for it in the report.
	 * Returns: The user's type and its object path, or its package name when the registry has no asset for it.
	 */
	static FMaterialUser MakeMaterialUser(IAssetRegistry& AssetRegistry, const FName ReferencerPackage)
	{
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(ReferencerPackage, PackageAssets);

		FMaterialUser User;
		User.Type = ClassifyReferencer(AssetRegistry, ReferencerPackage, PackageAssets);
		User.Path = PackageAssets.Num() > 0
			? PackageAssets[0].GetObjectPathString()
			: ReferencerPackage.ToString();
		return User;
	}

	/** Returns the object path of InstanceData's parent material, read from its registry tag. */
	static FString GetParentPath(const FAssetData& InstanceData)
	{
		FString ParentExportTextPath;
		InstanceData.GetTagValue(TEXT("Parent"), ParentExportTextPath);
		return FPackageName::ExportTextPathToObjectPath(ParentExportTextPath);
	}

	/** One way a loaded user draws materials: the usage flag it needs and the materials it draws that way. */
	struct FMaterialUse
	{
		/** Usage flag needed, or MATUSAGE_MAX when the component or emitter type is not recognised. */
		EMaterialUsage Usage = MATUSAGE_MAX;

		/** Materials drawn this way. Empty means the use applies to every material of the user. */
		TArray<FSoftObjectPath> Materials;
	};

	/** Adds a use of Usage for the valid entries of Materials to Uses. */
	static void AddMaterialUse(
		TArray<FMaterialUse>& Uses,
		const EMaterialUsage Usage,
		const TArray<UMaterialInterface*>& Materials)
	{
		FMaterialUse& Use = Uses.AddDefaulted_GetRef();
		Use.Usage = Usage;
		for (const UMaterialInterface* Material : Materials)
		{
			if (Material != nullptr) Use.Materials.AddUnique(FSoftObjectPath(Material));
		}
	}

	/**
	 * Returns the usage flag a component needs for the materials it draws.
	 * Returns: MATUSAGE_MAX for component types this report does not recognize.
	 */
	static EMaterialUsage GetComponentUsage(const UPrimitiveComponent& Component)
	{
		// Subclasses are checked before UStaticMeshComponent because they derive from it.
		if (Component.IsA<USplineMeshComponent>()) return MATUSAGE_SplineMesh;
		if (Component.IsA<UInstancedStaticMeshComponent>()) return MATUSAGE_InstancedStaticMeshes;
		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(&Component))
		{
			const UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
			return StaticMesh != nullptr && StaticMesh->IsNaniteEnabled() ? MATUSAGE_Nanite : MATUSAGE_StaticMesh;
		}
		if (Component.IsA<USkeletalMeshComponent>()) return MATUSAGE_SkeletalMesh;

		// Compared by path, so the GeometryCollectionEngine module is not needed as a dependency.
		static const FTopLevelAssetPath GeometryCollectionComponentClass(
			TEXT("/Script/GeometryCollectionEngine"), TEXT("GeometryCollectionComponent"));
		for (const UClass* Class = Component.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
		{
			if (Class->GetClassPathName() == GeometryCollectionComponentClass) return MATUSAGE_GeometryCollections;
		}
		return MATUSAGE_MAX;
	}

	/**
	 * Lists the materials drawn by every primitive component of a Blueprint, including inherited ones.
	 * Input: Blueprint must be loaded. Components overridden in child Blueprints are read from their parent.
	 * Returns: One use per component.
	 */
	static TArray<FMaterialUse> FindBlueprintUses(const UBlueprint& Blueprint)
	{
		TArray<const UActorComponent*> ComponentTemplates;
		for (UClass* Class = Blueprint.GeneratedClass; Class != nullptr; Class = Class->GetSuperClass())
		{
			const UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Class);
			if (BlueprintClass == nullptr || BlueprintClass->SimpleConstructionScript == nullptr) continue;
			for (const USCS_Node* Node : BlueprintClass->SimpleConstructionScript->GetAllNodes())
			{
				if (Node != nullptr && Node->ComponentTemplate != nullptr) ComponentTemplates.Add(Node->ComponentTemplate);
			}
		}

		// Components created in native C++ constructors live on the class default object instead.
		if (Blueprint.GeneratedClass != nullptr)
		{
			if (const AActor* DefaultActor = Cast<AActor>(Blueprint.GeneratedClass->GetDefaultObject()))
			{
				for (const UActorComponent* Component : DefaultActor->GetComponents())
				{
					if (Component != nullptr) ComponentTemplates.Add(Component);
				}
			}
		}

		TArray<FMaterialUse> Uses;
		for (const UActorComponent* ComponentTemplate : ComponentTemplates)
		{
			const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(ComponentTemplate);
			if (PrimitiveComponent == nullptr) continue;

			TArray<UMaterialInterface*> UsedMaterials;
			PrimitiveComponent->GetUsedMaterials(UsedMaterials);
			AddMaterialUse(Uses, GetComponentUsage(*PrimitiveComponent), UsedMaterials);
		}
		return Uses;
	}

	/**
	 * Lists the materials drawn by each emitter of a Cascade particle system, using its highest-detail LOD.
	 * Returns: One use per emitter, with the usage flag its emitter type needs.
	 */
	static TArray<FMaterialUse> FindParticleSystemUses(const UParticleSystem& ParticleSystem)
	{
		TArray<FMaterialUse> Uses;
		for (const UParticleEmitter* Emitter : ParticleSystem.Emitters)
		{
			if (Emitter == nullptr || Emitter->LODLevels.Num() == 0 || Emitter->LODLevels[0] == nullptr) continue;
			const UParticleLODLevel& LODLevel = *Emitter->LODLevels[0];

			TArray<UMaterialInterface*> EmitterMaterials;
			if (LODLevel.RequiredModule != nullptr) EmitterMaterials.Add(LODLevel.RequiredModule->Material);
			for (const UParticleModule* Module : LODLevel.Modules)
			{
				if (const UParticleModuleMeshMaterial* MeshMaterialModule = Cast<UParticleModuleMeshMaterial>(Module))
				{
					EmitterMaterials.Append(MeshMaterialModule->MeshMaterials);
				}
			}

			// Emitters without a type data module draw sprites.
			EMaterialUsage Usage = MATUSAGE_ParticleSprites;
			const UParticleModuleTypeDataBase* TypeData = LODLevel.TypeDataModule;
			if (const UParticleModuleTypeDataMesh* MeshTypeData = Cast<UParticleModuleTypeDataMesh>(TypeData))
			{
				Usage = MATUSAGE_MeshParticles;
				if (MeshTypeData->Mesh != nullptr)
				{
					for (const FStaticMaterial& StaticMaterial : MeshTypeData->Mesh->GetStaticMaterials())
					{
						EmitterMaterials.Add(StaticMaterial.MaterialInterface);
					}
				}
			}
			else if (TypeData != nullptr &&
				(TypeData->IsA<UParticleModuleTypeDataBeam2>() ||
				TypeData->IsA<UParticleModuleTypeDataRibbon>() ||
				TypeData->IsA<UParticleModuleTypeDataAnimTrail>()))
			{
				Usage = MATUSAGE_BeamTrails;
			}
			AddMaterialUse(Uses, Usage, EmitterMaterials);
		}
		return Uses;
	}

	/**
	 * Lists the skeletal mesh features that need their own usage flags. They apply to every material of the
	 * mesh, because the report does not check which sections have morph targets or cloth.
	 * Returns: One use per feature found.
	 */
	static TArray<FMaterialUse> FindSkeletalMeshUses(USkeletalMesh& SkeletalMesh)
	{
		TArray<FMaterialUse> Uses;
		if (SkeletalMesh.GetMorphTargets().Num() > 0) Uses.Add({ MATUSAGE_MorphTargets, {} });
		if (SkeletalMesh.HasActiveClothingAssets()) Uses.Add({ MATUSAGE_Clothing, {} });
		if (SkeletalMesh.GetDefaultMeshDeformer() != nullptr) Uses.Add({ MATUSAGE_MeshDeformer, {} });
		return Uses;
	}

	/**
	 * Reads the package header at PackageName, without creating any of its objects, and checks whether one of its
	 * objects has the class named ClassName.
	 * Input: The package must have no objects or open header in memory, so the header is not shared with a loaded
	 * or loading package.
	 * Output: bOutFound says whether such an object exists. The header is closed again; the empty package object
	 * it leaves is unloaded with the batch's other packages.
	 * Returns: False when the header cannot be read.
	 */
	static bool TryFindPackageObjectOfClass(const FName PackageName, const FName ClassName, bool& bOutFound)
	{
		FPackagePath PackagePath;
		if (!FPackagePath::TryFromPackageName(PackageName, PackagePath)) return false;
		FLinkerLoad* Linker = GetPackageLinker(nullptr, PackagePath, LOAD_NoVerify | LOAD_Quiet | LOAD_NoWarn, nullptr);
		if (Linker == nullptr) return false;

		bOutFound = Linker->ExportMap.ContainsByPredicate([Linker, ClassName](const FObjectExport& Export)
		{
			return !Export.ClassIndex.IsNull() && Linker->ImpExp(Export.ClassIndex).ObjectName == ClassName;
		});
		ResetLoaders(Linker->LinkerRoot);
		return true;
	}

	/** Returns whether the package at PackageName hard-references a mesh deformer asset, such as a Deformer Graph. */
	static bool HasMeshDeformerDependency(IAssetRegistry& AssetRegistry, const FName PackageName)
	{
		TSet<FTopLevelAssetPath> DeformerClasses;
		AssetRegistry.GetDerivedClassNames({ UMeshDeformer::StaticClass()->GetClassPathName() }, {}, DeformerClasses);

		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(
			PackageName,
			Dependencies,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));
		for (const FName Dependency : Dependencies)
		{
			TArray<FAssetData> DependencyAssets;
			AssetRegistry.GetAssetsByPackageName(Dependency, DependencyAssets);
			for (const FAssetData& DependencyAsset : DependencyAssets)
			{
				if (DeformerClasses.Contains(DependencyAsset.AssetClassPath)) return true;
			}
		}
		return false;
	}

	/**
	 * Finds the same uses as FindSkeletalMeshUses for a skeletal mesh that is not loaded, without loading it, so
	 * its render data, skeleton, physics asset, materials and textures stay on disk. Morph targets come from the
	 * Asset Registry tag, the mesh deformer from the mesh's dependencies, and clothing from its package header.
	 * Clothing assets count even when no mesh section uses them, unlike USkeletalMesh::HasActiveClothingAssets.
	 * Output: Fills OutUses when it returns true.
	 * Returns: False when the asset is not an unloaded skeletal mesh or its data cannot be read this way, so the
	 * caller loads it instead.
	 */
	static bool TryFindUnloadedSkeletalMeshUses(IAssetRegistry& AssetRegistry, const FSoftObjectPath& UserPath, TArray<FMaterialUse>& OutUses)
	{
		const FAssetData UserData = AssetRegistry.GetAssetByObjectPath(UserPath);
		if (!UserData.IsValid() || UserData.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName()) return false;
		// Packages saved before the tag existed, or already in memory, are left to the loading path.
		int32 MorphTargetCount = 0;
		if (!UserData.GetTagValue(TEXT("MorphTargets"), MorphTargetCount)) return false;
		if (IsAsyncLoading()) return false;
		if (UPackage* ExistingPackage = FindPackage(nullptr, *UserData.PackageName.ToString()))
		{
			// An empty package left by an earlier header read can be read again; a package with objects cannot.
			bool bHasObjects = false;
			ForEachObjectWithPackage(ExistingPackage, [&bHasObjects](UObject*)
			{
				bHasObjects = true;
				return false;
			}, EGetObjectsFlags::None);
			if (bHasObjects || ExistingPackage->GetLinker() != nullptr) return false;
		}

		bool bHasClothing = false;
		if (!TryFindPackageObjectOfClass(UserData.PackageName, TEXT("ClothingAssetCommon"), bHasClothing)) return false;

		OutUses.Reset();
		if (MorphTargetCount > 0) OutUses.Add({ MATUSAGE_MorphTargets, {} });
		if (bHasClothing) OutUses.Add({ MATUSAGE_Clothing, {} });
		if (HasMeshDeformerDependency(AssetRegistry, UserData.PackageName)) OutUses.Add({ MATUSAGE_MeshDeformer, {} });
		return true;
	}

	/** Gathers the Material Analytics report over several editor ticks and saves it when done. */
	class FMaterialAnalyticsRunner
	{
	public:
		/** Starts gathering unless a report is already running. */
		void Start()
		{
			if (TickerHandle.IsValid()) return;
			ResetReportData();
			PackageUnloader.Reset();
			ProgressNotification.Show(
				LOCTEXT("PreparingMaterialAnalytics", "Preparing Material Analytics..."),
				LOCTEXT("CancelMaterialAnalyticsTooltip", "Stop gathering material analytics."),
				FSimpleDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Cancel));

			// Work is split into small steps run on separate ticks, so the editor repaints the notification in between.
			Step = ERunnerStep::ShowNotification;
			StepStartSeconds = FPlatformTime::Seconds();
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Tick));
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			AbortRun();
			ProgressNotification.Dismiss();
		}

	private:
		/** Stages of a report run. Each Tick performs one stage, or part of ProcessMaterials. */
		enum class ERunnerStep : uint8
		{
			/** Waits briefly so the notification is visible before any blocking work. */
			ShowNotification,
			/** Waits without blocking until the Asset Registry has finished its startup scan. */
			WaitForAssetRegistry,
			/** Scans the search folders and lists the master materials. */
			FindMaterials,
			/** Maps every material instance to its parent. */
			BuildInstanceTree,
			/** Fills report rows within a per-tick time budget. */
			ProcessMaterials,
			/** Unloads the packages loaded by the finished batch. */
			UnloadPackages
		};

		/** Updates the notification with the current material count and the last material processed. */
		void UpdateProgressNotification() const
		{
			const int32 ProgressPercent = PendingAssets.Num() > 0
				? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
				: 100;
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("MaterialAnalyticsProgress", "Gathering Material Analytics - {0} / {1} ({2}%)\n{3}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(ProgressPercent),
				FText::FromString(CurrentAssetName)));
		}

		/** Clears pending assets, finished rows and the instance tree. */
		void ResetReportData()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			ChildInstancesByParent.Reset();
			InstancePackages.Reset();
			UserByPackage.Reset();
			UsesByUserPath.Reset();
			NextAssetIndex = 0;
			MaterialsInBatch = 0;
			BatchLoadedAssetCount = 0;
			PackagesBeforeBatch.Reset();
			CurrentAssetName.Reset();
		}

		/** Adds the packages loaded by a batch that was interrupted to the unloader, so they are not left loaded. */
		void TrackUnfinishedBatch()
		{
			if (MaterialsInBatch > 0 && BatchLoadedAssetCount > 0) PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
		}

		/** Stops ticking, throws away unfinished results, and unloads the packages the run loaded. */
		void AbortRun()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			AbortRun();
			ProgressNotification.Finish(
				LOCTEXT("MaterialAnalyticsCancelled", "Material Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/**
		 * Maps every material instance in the project to its parent, without loading anything.
		 * Instances often live outside the search folders, so the whole registry is used; Tick waits for the
		 * registry's startup scan to finish before this runs.
		 */
		void BuildInstanceTree(IAssetRegistry& AssetRegistry)
		{
			TArray<FAssetData> MaterialInstances;
			AssetRegistry.GetAssetsByClass(
				UMaterialInstanceConstant::StaticClass()->GetClassPathName(),
				MaterialInstances,
				true);
			for (const FAssetData& MaterialInstance : MaterialInstances)
			{
				const FSoftObjectPath ParentPath(GetParentPath(MaterialInstance));
				if (ParentPath.IsNull()) continue;
				ChildInstancesByParent.FindOrAdd(ParentPath).Add(MaterialInstance);
				InstancePackages.Add(MaterialInstance.PackageName);
			}
		}

		/**
		 * Finds the instances below MaterialPath, including instances of instances.
		 * Input: Settings decides whether unreferenced or Developers-only instances are skipped.
		 * Returns: The counted instances. A skipped instance has no counted children,
		 * because any child would itself be a reference to it.
		 */
		TArray<FAssetData> GatherCountedInstances(
			IAssetRegistry& AssetRegistry,
			const FSoftObjectPath& MaterialPath,
			const UAssetAnalyticsSettings& Settings) const
		{
			TSet<FSoftObjectPath> VisitedInstances;
			TArray<FSoftObjectPath> PathsToVisit;
			PathsToVisit.Add(MaterialPath);
			TArray<FAssetData> CountedInstances;
			while (PathsToVisit.Num() > 0)
			{
				const FSoftObjectPath CurrentPath = PathsToVisit.Pop(EAllowShrinking::No);
				const TArray<FAssetData>* ChildInstances = ChildInstancesByParent.Find(CurrentPath);
				if (ChildInstances == nullptr) continue;

				for (const FAssetData& ChildInstance : *ChildInstances)
				{
					// The visited check guards against broken data that loops back on itself.
					bool bAlreadyVisited = false;
					VisitedInstances.Add(ChildInstance.GetSoftObjectPath(), &bAlreadyVisited);
					if (bAlreadyVisited) continue;
					if (Settings.bIgnoreUnreferencedAssets &&
						!Core::HasRelevantReferencer(AssetRegistry, ChildInstance, Settings.bIgnoreDeveloperReferences)) continue;

					CountedInstances.Add(ChildInstance);
					PathsToVisit.Add(ChildInstance.GetSoftObjectPath());
				}
			}
			return CountedInstances;
		}

		/**
		 * Finds the distinct assets of each user type that reference the master or its counted instances.
		 * Material instances are skipped because the instance list already covers them.
		 * Output: Fills Item.UserPathsByType, one sorted list per EMaterialUserType.
		 */
		void GatherUsersByType(
			IAssetRegistry& AssetRegistry,
			const TArray<FAssetData>& MaterialAndInstances,
			const UAssetAnalyticsSettings& Settings,
			FMaterialAssetData& Item)
		{
			TSet<FName> UserPackages;
			for (const FAssetData& MaterialOrInstance : MaterialAndInstances)
			{
				TArray<FName> Referencers;
				AssetRegistry.GetReferencers(
					MaterialOrInstance.PackageName,
					Referencers,
					UE::AssetRegistry::EDependencyCategory::Package,
					UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Game));
				for (const FName Referencer : Referencers)
				{
					if (InstancePackages.Contains(Referencer)) continue;
					if (Settings.bIgnoreDeveloperReferences && Core::IsDeveloperPackage(Referencer)) continue;
					UserPackages.Add(Referencer);
				}
			}

			Item.UserPathsByType.SetNum(static_cast<int32>(EMaterialUserType::Count));
			for (const FName UserPackage : UserPackages)
			{
				const FMaterialUser* CachedUser = UserByPackage.Find(UserPackage);
				const FMaterialUser& User = CachedUser != nullptr
					? *CachedUser
					: UserByPackage.Add(UserPackage, MakeMaterialUser(AssetRegistry, UserPackage));
				Item.UserPathsByType[static_cast<int32>(User.Type)].Add(User.Path);
			}
			for (TArray<FString>& UserPaths : Item.UserPathsByType)
			{
				UserPaths.Sort();
			}
		}

		/** Scans the configured folders and lists their master materials in PendingAssets. */
		void FindMasterMaterials(IAssetRegistry& AssetRegistry)
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FARFilter Filter;
			// Subclasses are left out so material instances are not reported as masters.
			Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			for (const FDirectoryPath& Directory : Settings->MaterialSearchFolders)
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
						TEXT("Asset Analytics skipped material search folder '%s' because it is invalid or its content root is not mounted."),
						*Directory.Path);
				}
			}
			if (Filter.PackagePaths.Num() == 0) return;

			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);
		}

		/** Returns whether the enabled settings need the material instance tree. */
		static bool NeedsInstanceTree(const UAssetAnalyticsSettings& Settings)
		{
			return Settings.bMaterialInstanceCounts || Settings.bMaterialUserTypeCounts || Settings.bMaterialShaderPermutations;
		}

		/** Returns whether the enabled settings load assets, so batches must be unloaded. */
		static bool NeedsAssetLoading(const UAssetAnalyticsSettings& Settings)
		{
			return Settings.bMaterialUsageFlags ||
				Settings.bMaterialShaderPermutations ||
				(Settings.bMaterialUserTypeCounts && Settings.bMaterialResolveUsersByLoading);
		}

		/**
		 * Builds the shader map key of Material: its effective static parameters with every bOverride cleared, as
		 * FMaterialShaderMapId::UpdateFromParameterSet does, so an inherited value and an overridden equal value match.
		 */
		static FStaticPermutationKey MakePermutationKey(UMaterialInterface& Material, const uint64 BasePropertyOverridesHash)
		{
			FStaticPermutationKey Key;
			Material.GetStaticParameterValues(Key.StaticParameters);
			for (FStaticSwitchParameter& SwitchParameter : Key.StaticParameters.StaticSwitchParameters)
			{
				SwitchParameter.bOverride = false;
			}
			for (FStaticComponentMaskParameter& MaskParameter : Key.StaticParameters.EditorOnly.StaticComponentMaskParameters)
			{
				MaskParameter.bOverride = false;
			}
			Key.BasePropertyOverridesHash = BasePropertyOverridesHash;
			return Key;
		}

		/** Returns mask channels as report text, such as "R_B_" for red and blue. */
		static FString FormatComponentMask(const bool bR, const bool bG, const bool bB, const bool bA)
		{
			return FString::Printf(TEXT("%s%s%s%s"), bR ? TEXT("R") : TEXT("_"), bG ? TEXT("G") : TEXT("_"), bB ? TEXT("B") : TEXT("_"), bA ? TEXT("A") : TEXT("_"));
		}

		/**
		 * Lists the master's static switches, static component masks and overridable base properties with their
		 * defaults, ready for instance overrides to be counted against them.
		 * Output: Fills Item.PermutationParameters.
		 */
		static void AddPermutationParameters(const UMaterial& MasterMaterial, FMaterialAssetData& Item)
		{
			TArray<FMaterialParameterInfo> ParameterInfos;
			TArray<FGuid> ParameterGuids;
			MasterMaterial.GetAllParameterInfoOfType(EMaterialParameterType::StaticSwitch, ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& SwitchInfo : ParameterInfos)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::StaticSwitch;
				Summary.ParameterInfo = SwitchInfo;
				bool bDefaultValue = false;
				FGuid DefaultExpressionGuid;
				MasterMaterial.GetStaticSwitchParameterDefaultValue(FHashedMaterialParameterInfo(SwitchInfo), bDefaultValue, DefaultExpressionGuid);
				Summary.DefaultValue = bDefaultValue ? TEXT("True") : TEXT("False");
			}

			ParameterInfos.Reset();
			ParameterGuids.Reset();
			MasterMaterial.GetAllParameterInfoOfType(EMaterialParameterType::StaticComponentMask, ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& MaskInfo : ParameterInfos)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::ComponentMask;
				Summary.ParameterInfo = MaskInfo;
				bool bR = false, bG = false, bB = false, bA = false;
				FGuid DefaultExpressionGuid;
				MasterMaterial.GetStaticComponentMaskParameterDefaultValue(FHashedMaterialParameterInfo(MaskInfo), bR, bG, bB, bA, DefaultExpressionGuid);
				Summary.DefaultValue = FormatComponentMask(bR, bG, bB, bA);
			}

			for (const FBasePropertyCheck& BaseProperty : BasePropertyChecks)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::BaseProperty;
				Summary.ParameterInfo.Name = BaseProperty.Name;
				Summary.DefaultValue = BaseProperty.ValueText(MasterMaterial);
			}
		}

		/**
		 * Compares the instance's static parameters and base properties with the master and its parent. Overrides
		 * inherited from a parent instance are counted on that parent, so each override is counted once.
		 * Input: Instance must be loaded and have a parent. InstanceKey is the instance's shader map key, which holds
		 * the static parameter values it ends up with.
		 * Output: Increases ChangedCount for own overrides that differ from the parent's value, including ones that
		 * restore the master's value, and NoOpCount for own overrides that repeat the parent's value. Adds a cell to
		 * Row for each parameter the instance overrides, or inherits with a value different from the master's.
		 */
		static void ReadInstancePermutationCells(
			const UMaterial& MasterMaterial,
			const UMaterialInstance& Instance,
			const FStaticPermutationKey& InstanceKey,
			FMaterialAssetData& Item,
			FInstancePermutationRow& Row)
		{
			const UMaterialInterface& Parent = *Instance.Parent;
			const FStaticParameterSet& OwnStaticParameters = Instance.GetStaticParameters();
			auto FindParameterIndex = [&Item](const EPermutationParameterKind Kind, const FMaterialParameterInfo& ParameterInfo)
			{
				return Item.PermutationParameters.IndexOfByPredicate([Kind, &ParameterInfo](const FPermutationParameterSummary& Candidate)
				{
					return Candidate.Kind == Kind && Candidate.ParameterInfo == ParameterInfo;
				});
			};
			auto AddCell = [&Item, &Row](const int32 ParameterIndex, const FString& Value, const bool bOwnOverride, const bool bDiffersFromMaster, const bool bSameAsParent)
			{
				if (ParameterIndex == INDEX_NONE) return;
				FPermutationParameterSummary& Summary = Item.PermutationParameters[ParameterIndex];
				if (bOwnOverride && !bSameAsParent) ++Summary.ChangedCount;
				if (bOwnOverride && bSameAsParent) ++Summary.NoOpCount;
				// Inherited parameters with the master's value need no cell; the report shows them with the master's default.
				if (!bOwnOverride && !bDiffersFromMaster) return;
				FInstancePermutationCell& Cell = Row.Cells.AddDefaulted_GetRef();
				Cell.ParameterIndex = ParameterIndex;
				Cell.Value = Value;
				Cell.bOwnOverride = bOwnOverride;
				Cell.bSameAsParent = bOwnOverride && bSameAsParent;
			};

			for (const FStaticSwitchParameter& SwitchParameter : InstanceKey.StaticParameters.StaticSwitchParameters)
			{
				const int32 ParameterIndex = FindParameterIndex(EPermutationParameterKind::StaticSwitch, SwitchParameter.ParameterInfo);
				const FString ValueText = SwitchParameter.Value ? TEXT("True") : TEXT("False");
				const bool bOwnOverride = OwnStaticParameters.StaticSwitchParameters.ContainsByPredicate([&SwitchParameter](const FStaticSwitchParameter& OwnParameter)
				{
					return OwnParameter.bOverride && OwnParameter.ParameterInfo == SwitchParameter.ParameterInfo;
				});
				FMaterialParameterMetadata ParentValue;
				const bool bSameAsParent =
					Parent.GetParameterValue(EMaterialParameterType::StaticSwitch, SwitchParameter.ParameterInfo, ParentValue) &&
					ParentValue.Value.AsStaticSwitch() == SwitchParameter.Value;
				const bool bDiffersFromMaster = ParameterIndex != INDEX_NONE && Item.PermutationParameters[ParameterIndex].DefaultValue != ValueText;
				AddCell(ParameterIndex, ValueText, bOwnOverride, bDiffersFromMaster, bSameAsParent);
			}
			for (const FStaticComponentMaskParameter& MaskParameter : InstanceKey.StaticParameters.EditorOnly.StaticComponentMaskParameters)
			{
				const int32 ParameterIndex = FindParameterIndex(EPermutationParameterKind::ComponentMask, MaskParameter.ParameterInfo);
				const FString MaskText = FormatComponentMask(MaskParameter.R, MaskParameter.G, MaskParameter.B, MaskParameter.A);
				const bool bOwnOverride = OwnStaticParameters.EditorOnly.StaticComponentMaskParameters.ContainsByPredicate([&MaskParameter](const FStaticComponentMaskParameter& OwnParameter)
				{
					return OwnParameter.bOverride && OwnParameter.ParameterInfo == MaskParameter.ParameterInfo;
				});
				FMaterialParameterMetadata ParentValue;
				const bool bSameAsParent =
					Parent.GetParameterValue(EMaterialParameterType::StaticComponentMask, MaskParameter.ParameterInfo, ParentValue) &&
					FormatComponentMask(ParentValue.Value.Bool[0], ParentValue.Value.Bool[1], ParentValue.Value.Bool[2], ParentValue.Value.Bool[3]) == MaskText;
				const bool bDiffersFromMaster = ParameterIndex != INDEX_NONE && Item.PermutationParameters[ParameterIndex].DefaultValue != MaskText;
				AddCell(ParameterIndex, MaskText, bOwnOverride, bDiffersFromMaster, bSameAsParent);
			}
			for (const FBasePropertyCheck& BaseProperty : BasePropertyChecks)
			{
				FMaterialParameterInfo PropertyInfo;
				PropertyInfo.Name = BaseProperty.Name;
				const bool bDiffersFromMaster = !BaseProperty.HasSameValue(Instance, MasterMaterial);
				AddCell(
					FindParameterIndex(EPermutationParameterKind::BaseProperty, PropertyInfo),
					BaseProperty.ValueText(Instance),
					BaseProperty.IsOverridden(Instance.BasePropertyOverrides),
					bDiffersFromMaster,
					BaseProperty.HasSameValue(Instance, Parent));
			}
		}

		/**
		 * Loads a master and its counted instances to read their static permutations on the editor's shader platform.
		 * Instances that override static parameters or base properties compile their own shader map. Instances whose
		 * shader map key matches the master's or another instance's compile the same shaders, so the layout shader
		 * count is taken once per unique permutation, repeated for each of that instance's quality shader maps.
		 * Output: Fills Item.PermutationParameters, InstancePermutationRows, StaticPermutationInstancePaths and
		 * LayoutShaderCount, and sets Item.bShaderPermutationsRead.
		 * Returns: The number of materials loaded from disk.
		 */
		static int32 ReadStaticPermutations(const FAssetData& MasterData, FMaterialAssetData& Item)
		{
			int32 LoadedAssetCount = 0;
			const bool bWasMasterLoaded = MasterData.IsAssetLoaded();
			UMaterial* MasterMaterial = Cast<UMaterial>(MasterData.GetAsset());
			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to read its static permutations."), *MasterData.GetObjectPathString());
				return LoadedAssetCount;
			}
			if (!bWasMasterLoaded) ++LoadedAssetCount;

			const EShaderPlatform ShaderPlatform = GMaxRHIShaderPlatform;
			// The cooked game for the editor's own OS, such as Windows, so editor-only shaders are left out of the layout.
			const ITargetPlatform* CookedPlatform = GetTargetPlatformManagerRef().FindTargetPlatform(FPlatformProperties::IniPlatformName());
			int32 LayoutShaderCount = CountLayoutShaders(*MasterMaterial, ShaderPlatform, CookedPlatform, 1);

			AddPermutationParameters(*MasterMaterial, Item);

			// The master's base properties are its own, so its base property hash is 0, as for instances that change none.
			const FStaticPermutationKey MasterKey = MakePermutationKey(*MasterMaterial, 0);
			TArray<FStaticPermutationKey> PermutationKeys;
			for (const FAssetData& InstanceData : Item.Instances)
			{
				const bool bWasAssetLoaded = InstanceData.IsAssetLoaded();
				UMaterialInstance* Instance = Cast<UMaterialInstance>(InstanceData.GetAsset());
				if (Instance == nullptr)
				{
					UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material instance %s to read its static permutation."), *InstanceData.GetObjectPathString());
					continue;
				}
				if (!bWasAssetLoaded) ++LoadedAssetCount;

				FStaticPermutationKey Key = MakePermutationKey(*Instance, Instance->GetBasePropertyOverridesHash());
				FInstancePermutationRow& Row = Item.InstancePermutationRows.AddDefaulted_GetRef();
				Row.Path = InstanceData.GetObjectPathString();
				if (Instance->Parent != nullptr) ReadInstancePermutationCells(*MasterMaterial, *Instance, Key, Item, Row);

				if (!Instance->bHasStaticPermutationResource) continue;

				Item.StaticPermutationInstancePaths.Add(InstanceData.GetObjectPathString());
				// Instances whose key matches the master's or an earlier instance's share that shader map.
				const bool bAlreadySeen = MasterKey.CompilesSameShaders(Key) ||
					PermutationKeys.ContainsByPredicate([&Key](const FStaticPermutationKey& SeenKey)
					{
						return SeenKey.CompilesSameShaders(Key);
					});
				if (bAlreadySeen) continue;

				PermutationKeys.Add(MoveTemp(Key));
				// The layout only covers the instance's active quality level, so it is repeated for each quality shader map.
				// GetShaderTypes asserts that static permutation instances are constant instances.
				if (Instance->IsA<UMaterialInstanceConstant>())
				{
					const int32 InstanceShaderMapCount = CountQualityShaderMaps(*Instance, ShaderPlatform);
					LayoutShaderCount += CountLayoutShaders(*Instance, ShaderPlatform, CookedPlatform, InstanceShaderMapCount);
				}
			}

			Item.InstancePermutationRows.Sort([](const FInstancePermutationRow& Left, const FInstancePermutationRow& Right)
			{
				return Left.Path < Right.Path;
			});

			Item.StaticPermutationInstancePaths.Sort();
			Item.LayoutShaderCount = LayoutShaderCount;
			Item.bShaderPermutationsRead = true;
			return LoadedAssetCount;
		}

		/**
		 * Loads AssetData's master material and records which usage flags it has enabled.
		 * Output: Fills Item.UsageEnabled, or leaves it empty when the material cannot be loaded.
		 * Returns: True when this call loaded the material from disk.
		 */
		static bool ReadUsageFlags(const FAssetData& AssetData, FMaterialAssetData& Item)
		{
			const bool bWasAssetLoaded = AssetData.IsAssetLoaded();
			const UMaterial* MasterMaterial = Cast<UMaterial>(AssetData.GetAsset());
			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to read its usage flags."), *AssetData.GetObjectPathString());
				return false;
			}

			Item.UsageEnabled.SetNum(MATUSAGE_MAX);
			for (int32 UsageIndex = 0; UsageIndex < MATUSAGE_MAX; ++UsageIndex)
			{
				Item.UsageEnabled[UsageIndex] = MasterMaterial->GetUsageByFlag(static_cast<EMaterialUsage>(UsageIndex));
			}
			return !bWasAssetLoaded;
		}

		/**
		 * Returns the material uses of the user at UserPath, reading it the first time it is needed in this run.
		 * Skeletal meshes that are not loaded are read without loading them; other users are loaded.
		 * Output: Increases LoadedAssetCount when this call loaded the user from disk.
		 */
		const TArray<FMaterialUse>& GetUserUses(const FString& UserPath, int32& LoadedAssetCount)
		{
			if (const TArray<FMaterialUse>* CachedUses = UsesByUserPath.Find(UserPath)) return *CachedUses;

			const FSoftObjectPath UserObjectPath(UserPath);
			UObject* User = UserObjectPath.ResolveObject();
			if (User == nullptr)
			{
				TArray<FMaterialUse> SkeletalMeshUses;
				if (TryFindUnloadedSkeletalMeshUses(IAssetRegistry::GetChecked(), UserObjectPath, SkeletalMeshUses))
				{
					return UsesByUserPath.Add(UserPath, MoveTemp(SkeletalMeshUses));
				}
				User = UserObjectPath.TryLoad();
				if (User != nullptr) ++LoadedAssetCount;
			}

			TArray<FMaterialUse> Uses;
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(User)) Uses = FindBlueprintUses(*Blueprint);
			else if (const UParticleSystem* ParticleSystem = Cast<UParticleSystem>(User)) Uses = FindParticleSystemUses(*ParticleSystem);
			else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(User)) Uses = FindSkeletalMeshUses(*SkeletalMesh);
			else UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material user %s."), *UserPath);
			return UsesByUserPath.Add(UserPath, MoveTemp(Uses));
		}

		/**
		 * Reads the Blueprint, Cascade particle system and skeletal mesh users of a material and files each one
		 * under the usage flags it needs. Blueprints and particle systems are loaded; skeletal meshes are read
		 * without loading when possible (see GetUserUses). Blueprints and particle systems whose uses of this material are all
		 * recognised are removed from their broad group; skeletal meshes stay under SkeletalMesh.
		 * Input: MaterialAndInstances is the master and its counted instances.
		 * Returns: The number of users loaded from disk.
		 */
		int32 ResolveUsersByLoading(FMaterialAssetData& Item, const TArray<FAssetData>& MaterialAndInstances)
		{
			TSet<FSoftObjectPath> TreeMaterials;
			for (const FAssetData& MaterialOrInstance : MaterialAndInstances)
			{
				TreeMaterials.Add(MaterialOrInstance.GetSoftObjectPath());
			}

			int32 LoadedAssetCount = 0;
			Item.ResolvedUserPathsByUsage.SetNum(MATUSAGE_MAX);
			const EMaterialUserType ResolvableTypes[] =
			{
				EMaterialUserType::Blueprint,
				EMaterialUserType::Particle,
				EMaterialUserType::SkeletalMesh
			};
			for (const EMaterialUserType UserType : ResolvableTypes)
			{
				TArray<FString>& UserPaths = Item.UserPathsByType[static_cast<int32>(UserType)];
				for (int32 UserIndex = UserPaths.Num() - 1; UserIndex >= 0; --UserIndex)
				{
					const FString UserPath = UserPaths[UserIndex];
					TSet<EMaterialUsage> NeededUsages;
					bool bHasUnrecognisedUse = false;
					for (const FMaterialUse& Use : GetUserUses(UserPath, LoadedAssetCount))
					{
						const bool bUsesThisMaterial = Use.Materials.Num() == 0 ||
							Use.Materials.ContainsByPredicate([&TreeMaterials](const FSoftObjectPath& Material)
							{
								return TreeMaterials.Contains(Material);
							});
						if (!bUsesThisMaterial) continue;
						if (Use.Usage == MATUSAGE_MAX) bHasUnrecognisedUse = true;
						else NeededUsages.Add(Use.Usage);
					}

					for (const EMaterialUsage Usage : NeededUsages)
					{
						Item.ResolvedUserPathsByUsage[Usage].AddUnique(UserPath);
					}
					const bool bFullyResolved = NeededUsages.Num() > 0 && !bHasUnrecognisedUse;
					if (UserType != EMaterialUserType::SkeletalMesh && bFullyResolved) UserPaths.RemoveAt(UserIndex);
				}
			}
			for (TArray<FString>& ResolvedUserPaths : Item.ResolvedUserPathsByUsage)
			{
				ResolvedUserPaths.Sort();
			}
			return LoadedAssetCount;
		}

		/**
		 * Fills the report row of one master material, loading it or its users when the settings ask for it.
		 * Returns: The number of assets this call loaded from disk.
		 */
		int32 ProcessMaterial(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings, const FAssetData& AssetData)
		{
			if (Settings.bIgnoreUnreferencedAssets &&
				!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings.bIgnoreDeveloperReferences)) return 0;

			int32 LoadedAssetCount = 0;
			FMaterialAssetData& Item = AssetItems.Emplace_GetRef(AssetData);
			if (Settings.bMaterialPackageDiskSize)
			{
				FAssetPackageData PackageData;
				if (AssetRegistry.TryGetAssetPackageData(AssetData.PackageName, PackageData) ==
					UE::AssetRegistry::EExists::Exists)
				{
					Item.PackageDiskSize = PackageData.DiskSize;
				}
			}
			if (NeedsInstanceTree(Settings))
			{
				Item.Instances = GatherCountedInstances(AssetRegistry, AssetData.GetSoftObjectPath(), Settings);
				if (Settings.bMaterialUserTypeCounts)
				{
					TArray<FAssetData> MaterialAndInstances = Item.Instances;
					MaterialAndInstances.Add(AssetData);
					GatherUsersByType(AssetRegistry, MaterialAndInstances, Settings, Item);
					if (Settings.bMaterialResolveUsersByLoading) LoadedAssetCount += ResolveUsersByLoading(Item, MaterialAndInstances);
				}
			}
			if (Settings.bMaterialUsageFlags && ReadUsageFlags(AssetData, Item)) ++LoadedAssetCount;
			if (Settings.bMaterialShaderPermutations) LoadedAssetCount += ReadStaticPermutations(AssetData, Item);
			return LoadedAssetCount;
		}

		/**
		 * Processes master materials until the tick time budget is used, so the editor keeps repainting.
		 * AssetsPerTick masters make one batch; when a batch has loaded assets, the next tick unloads them.
		 * Returns: True while the report is still running.
		 */
		bool ProcessMaterialsForOneTick(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			const bool bLoadsAssets = NeedsAssetLoading(Settings);
			const int32 MaterialsPerBatch = FMath::Max(Settings.AssetsPerTick, 1);
			while (NextAssetIndex < PendingAssets.Num())
			{
				// Snapshot loaded packages so everything this batch loads, including dependencies, can be unloaded afterwards.
				if (bLoadsAssets && MaterialsInBatch == 0)
				{
					PackagesBeforeBatch = Core::FReportPackageUnloader::GetLoadedPackages();
					BatchStartSeconds = FPlatformTime::Seconds();
					BatchLoadedAssetCount = 0;
				}

				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				CurrentAssetName = AssetData.AssetName.ToString();
				BatchLoadedAssetCount += ProcessMaterial(AssetRegistry, Settings, AssetData);
				++NextAssetIndex;
				++MaterialsInBatch;

				const bool bBatchFinished = MaterialsInBatch >= MaterialsPerBatch || NextAssetIndex >= PendingAssets.Num();
				if (bBatchFinished)
				{
					if (BatchLoadedAssetCount > 0)
					{
						// Unloading can wait on shader compiles, so it gets its own tick and message.
						PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
						Step = ERunnerStep::UnloadPackages;
						ProgressNotification.SetText(FText::Format(
							LOCTEXT("UnloadingMaterialPackages", "Material Analytics - {0} / {1}\nUnloading {2} packages..."),
							FText::AsNumber(NextAssetIndex),
							FText::AsNumber(PendingAssets.Num()),
							FText::AsNumber(PackageUnloader.GetTrackedPackageCount())));
						return true;
					}
					MaterialsInBatch = 0;
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
				Step = NeedsInstanceTree(*Settings) ? ERunnerStep::WaitForAssetRegistry : ERunnerStep::FindMaterials;
				return true;

			case ERunnerStep::WaitForAssetRegistry:
				// Polling instead of WaitForCompletion lets the registry keep scanning while the editor stays responsive.
				if (AssetRegistry.IsLoadingAssets())
				{
					ProgressNotification.SetText(LOCTEXT("WaitingForAssetRegistry", "Material Analytics\nWaiting for the Asset Registry to finish scanning..."));
					return true;
				}
				Step = ERunnerStep::FindMaterials;
				ProgressNotification.SetText(LOCTEXT("ScanningMaterialFolders", "Material Analytics\nScanning material folders..."));
				return true;

			case ERunnerStep::FindMaterials:
				FindMasterMaterials(AssetRegistry);
				if (PendingAssets.Num() == 0)
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				if (NeedsInstanceTree(*Settings))
				{
					Step = ERunnerStep::BuildInstanceTree;
					ProgressNotification.SetText(LOCTEXT("BuildingInstanceTree", "Material Analytics\nBuilding the material instance tree..."));
					return true;
				}
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::BuildInstanceTree:
				BuildInstanceTree(AssetRegistry);
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::ProcessMaterials:
				return ProcessMaterialsForOneTick(AssetRegistry, *Settings);

			case ERunnerStep::UnloadPackages:
				PackageUnloader.UnloadReportPackages();
				UE_LOG(
					LogTemp,
					Log,
					TEXT("Material Analytics loaded and unloaded %d assets (masters, instances and users) for %d materials in %.2f s."),
					BatchLoadedAssetCount,
					MaterialsInBatch,
					FPlatformTime::Seconds() - BatchStartSeconds);
				MaterialsInBatch = 0;
				BatchLoadedAssetCount = 0;
				if (NextAssetIndex >= PendingAssets.Num())
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;
			}
			return true;
		}

		/** Sorts the results, saves the JSON report, and opens the browser report. */
		void CompleteReport()
		{
			AssetItems.Sort([](const FMaterialAssetData& Left, const FMaterialAssetData& Right)
			{
				const FString LeftName = Left.AssetData.AssetName.ToString();
				const FString RightName = Right.AssetData.AssetName.ToString();
				if (LeftName != RightName) return LeftName < RightName;
				return Left.AssetData.GetSoftObjectPath().LexicalLess(Right.AssetData.GetSoftObjectPath());
			});

			const FString ReportDirectory = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetAnalytics"));
			IFileManager::Get().MakeDirectory(*ReportDirectory, true);
			const FString JsonPath = FPaths::Combine(ReportDirectory, TEXT("MaterialAnalytics.json"));
			if (!FFileHelper::SaveStringToFile(
				BuildJsonData(),
				*JsonPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				ProgressNotification.Finish(
					LOCTEXT("MaterialJsonSaveFailed", "The material analytics JSON file could not be saved."),
					SNotificationItem::CS_Fail);
				ResetReportData();
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("Asset Analytics saved the Material Analytics report to %s"), *FPaths::ConvertRelativePathToFull(JsonPath));
			const FString ReportUrl = Core::GetMaterialAnalyticsReportUrl();
			FPlatformProcess::LaunchURL(*ReportUrl, nullptr, nullptr);
			ProgressNotification.Finish(
				FText::Format(
					LOCTEXT("MaterialAnalyticsComplete", "Material Analytics report saved for {0} master materials."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			ResetReportData();
		}

		/** Returns the JSON name of a permutation parameter kind. */
		static const TCHAR* GetPermutationParameterKindName(const EPermutationParameterKind Kind)
		{
			switch (Kind)
			{
			case EPermutationParameterKind::StaticSwitch: return TEXT("StaticSwitch");
			case EPermutationParameterKind::ComponentMask: return TEXT("ComponentMask");
			case EPermutationParameterKind::BaseProperty: return TEXT("BaseProperty");
			}
			return TEXT("Unknown");
		}

		/**
		 * Writes one material entry with the enabled fields.
		 * Output: Appends a JSON object to Writer, which must be inside the Materials array.
		 */
		static void WriteMaterialJson(
			TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>& Writer,
			const FMaterialAssetData& Item,
			const UAssetAnalyticsSettings& Settings)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("AssetName"), Item.AssetData.AssetName.ToString());
			Writer.WriteValue(TEXT("PackagePath"), Item.AssetData.PackagePath.ToString());
			if (Settings.bMaterialPackageDiskSize)
			{
				if (Item.PackageDiskSize >= 0) Writer.WriteValue(TEXT("PackageDiskSize"), Item.PackageDiskSize);
				else Writer.WriteNull(TEXT("PackageDiskSize"));
			}
			if (Settings.bMaterialInstanceCounts)
			{
				Writer.WriteValue(TEXT("InstanceCount"), Item.Instances.Num());
				Writer.WriteArrayStart(TEXT("Instances"));
				for (const FAssetData& Instance : Item.Instances)
				{
					Writer.WriteValue(Instance.GetObjectPathString());
				}
				Writer.WriteArrayEnd();
			}
			if (Settings.bMaterialUsageFlags)
			{
				// False when the master could not be loaded, so missing Enabled values are not read as "disabled".
				Writer.WriteValue(TEXT("UsageFlagsRead"), Item.UsageEnabled.Num() > 0);
			}
			if (Item.bShaderPermutationsRead)
			{
				// Shaders of the master and one instance per unique static permutation, on the editor's shader platform
				// with the cooked game's permutations.
				Writer.WriteValue(TEXT("ShaderCount"), Item.LayoutShaderCount);

				// ChangedCount: counted instances whose own override differs from their parent.
				// NoOpCount: counted instances whose own override repeats the value inherited from their parent.
				Writer.WriteArrayStart(TEXT("PermutationParameters"));
				for (const FPermutationParameterSummary& Parameter : Item.PermutationParameters)
				{
					Writer.WriteObjectStart();
					Writer.WriteValue(TEXT("Kind"), GetPermutationParameterKindName(Parameter.Kind));
					Writer.WriteValue(TEXT("Name"), Parameter.ParameterInfo.Name.ToString());
					if (!Parameter.DefaultValue.IsEmpty()) Writer.WriteValue(TEXT("Default"), Parameter.DefaultValue);
					Writer.WriteValue(TEXT("ChangedCount"), Parameter.ChangedCount);
					Writer.WriteValue(TEXT("NoOpCount"), Parameter.NoOpCount);
					Writer.WriteObjectEnd();
				}
				Writer.WriteArrayEnd();
				Writer.WriteArrayStart(TEXT("StaticPermutationInstances"));
				for (const FString& InstancePath : Item.StaticPermutationInstancePaths)
				{
					Writer.WriteValue(InstancePath);
				}
				Writer.WriteArrayEnd();

				// Cells: parameters the instance overrides or inherits with a value different from the master's.
				// Parameter is an index into PermutationParameters. State is SameAsParent for an own override that
				// repeats the parent's value, Changed for an own override that differs from the parent's value, and
				// Inherited for a value different from the master's that comes from a parent instance.
				Writer.WriteArrayStart(TEXT("InstancePermutations"));
				for (const FInstancePermutationRow& Row : Item.InstancePermutationRows)
				{
					Writer.WriteObjectStart();
					Writer.WriteValue(TEXT("Path"), Row.Path);
					Writer.WriteArrayStart(TEXT("Cells"));
					for (const FInstancePermutationCell& Cell : Row.Cells)
					{
						const TCHAR* State = Cell.bSameAsParent ? TEXT("SameAsParent")
							: Cell.bOwnOverride ? TEXT("Changed")
							: TEXT("Inherited");
						Writer.WriteObjectStart();
						Writer.WriteValue(TEXT("Parameter"), Cell.ParameterIndex);
						Writer.WriteValue(TEXT("Value"), Cell.Value);
						Writer.WriteValue(TEXT("State"), State);
						Writer.WriteObjectEnd();
					}
					Writer.WriteArrayEnd();
					Writer.WriteObjectEnd();
				}
				Writer.WriteArrayEnd();
			}
			if (Settings.bMaterialUserTypeCounts || Settings.bMaterialUsageFlags)
			{
				Writer.WriteObjectStart(TEXT("VertexFactories"));
				for (const FVertexFactoryEntry& Entry : VertexFactoryEntries)
				{
					WriteVertexFactoryJson(Writer, Entry, Item);
				}
				Writer.WriteObjectEnd();
			}
			Writer.WriteObjectEnd();
		}

		/**
		 * Returns why the users of Entry are unknown, as the JSON UsersUnknownReason value.
		 * Input: Entry has no Users list in Item.
		 */
		static const TCHAR* GetUsersUnknownReason(const FVertexFactoryEntry& Entry, const FMaterialAssetData& Item)
		{
			// Users were not gathered at all, so even registry users are unknown.
			if (Item.UserPathsByType.Num() == 0) return TEXT("UsersNotGathered");
			switch (Entry.UnregisteredUserLookup)
			{
			case EUnregisteredUserLookup::Loading:
				return Item.ResolvedUserPathsByUsage.Num() > 0 ? TEXT("NoneFoundByLoading") : TEXT("LoadingOff");
			case EUnregisteredUserLookup::PlacedActors: return TEXT("PlacedActors");
			case EUnregisteredUserLookup::Niagara: return TEXT("Niagara");
			case EUnregisteredUserLookup::NotChecked: return TEXT("NotChecked");
			}
			return TEXT("NotChecked");
		}

		/**
		 * Writes one VertexFactories entry when it has something to show: users, or an enabled usage flag.
		 * Users is written only when the users are known, so an empty list means "enabled but unused". Otherwise
		 * UsersUnknownReason says why they are unknown.
		 * Output: Appends Entry.Name and its object to Writer, or nothing when the entry is skipped.
		 */
		static void WriteVertexFactoryJson(
			TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>& Writer,
			const FVertexFactoryEntry& Entry,
			const FMaterialAssetData& Item)
		{
			const bool bHasUsageFlag = Entry.Usage != MATUSAGE_MAX && Item.UsageEnabled.Num() > 0;
			const bool bEnabled = bHasUsageFlag && Item.UsageEnabled[Entry.Usage];
			const bool bRegistryUsersKnown =
				Entry.RegistryUserType != EMaterialUserType::Count && Item.UserPathsByType.Num() > 0;

			// Users found by loading are added to the registry users. They only make Users known for entries the
			// registry cannot fill when at least one was found, because levels and placements were not checked.
			TArray<FString> UserPaths;
			if (bRegistryUsersKnown) UserPaths = Item.UserPathsByType[static_cast<int32>(Entry.RegistryUserType)];
			if (Entry.Usage != MATUSAGE_MAX && Item.ResolvedUserPathsByUsage.Num() > 0)
			{
				for (const FString& ResolvedUserPath : Item.ResolvedUserPathsByUsage[Entry.Usage])
				{
					UserPaths.AddUnique(ResolvedUserPath);
				}
				UserPaths.Sort();
			}
			const bool bUsersKnown = bRegistryUsersKnown || UserPaths.Num() > 0;
			if (UserPaths.Num() == 0 && !bEnabled) return;

			Writer.WriteObjectStart(Entry.Name);
			if (bHasUsageFlag) Writer.WriteValue(TEXT("Enabled"), bEnabled);
			if (bUsersKnown)
			{
				Writer.WriteArrayStart(TEXT("Users"));
				for (const FString& UserPath : UserPaths)
				{
					Writer.WriteValue(UserPath);
				}
				Writer.WriteArrayEnd();
			}
			else
			{
				// UsersNotGathered: Vertex Factory Counts was off. LoadingOff: Vertex Factory Users was off.
				// NoneFoundByLoading: loaded users found none, but levels were not checked. PlacedActors, Niagara
				// and NotChecked: the report cannot find these users.
				Writer.WriteValue(TEXT("UsersUnknownReason"), GetUsersUnknownReason(Entry, Item));
			}
			Writer.WriteObjectEnd();
		}

		/** Returns the JSON report text for the finished materials and the enabled fields. */
		FString BuildJsonData() const
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			Writer->WriteObjectStart();
			// The shader platform the counts are for. Index matches the Platform column of a cook's MaterialStats CSV.
			Writer->WriteObjectStart(TEXT("ShaderPlatform"));
			Writer->WriteValue(TEXT("Name"), FDataDrivenShaderPlatformInfo::GetName(GMaxRHIShaderPlatform).ToString());
			Writer->WriteValue(TEXT("Index"), static_cast<int32>(GMaxRHIShaderPlatform));
			Writer->WriteObjectEnd();
			Writer->WriteArrayStart(TEXT("Materials"));
			for (const FMaterialAssetData& Item : AssetItems)
			{
				WriteMaterialJson(*Writer, Item, *Settings);
			}
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
			Writer->Close();
			return JsonText;
		}

		/** How long the notification is shown before the first blocking step runs. */
		static constexpr double NotificationDisplayDelaySeconds = 0.2;
		/** Longest time one tick spends processing materials before letting the editor repaint. */
		static constexpr double TickTimeBudgetSeconds = 0.05;
		/** Report rows gathered so far, one per processed master material. */
		TArray<FMaterialAssetData> AssetItems;
		/** Master materials found in the configured folders, processed in order. */
		TArray<FAssetData> PendingAssets;
		/** Direct child instances for every material or instance that has any, keyed by the parent's object path. */
		TMap<FSoftObjectPath, TArray<FAssetData>> ChildInstancesByParent;
		/** Packages of every material instance in the instance tree, so they are not counted as users. */
		TSet<FName> InstancePackages;
		/** Type and report path of each referencing package classified so far in this run. */
		TMap<FName, FMaterialUser> UserByPackage;
		/** Material uses of each user loaded so far in this run, so every user is loaded only once. */
		TMap<FString, TArray<FMaterialUse>> UsesByUserPath;
		/** Unloads the packages each batch loaded. */
		Core::FReportPackageUnloader PackageUnloader;
		/** Ticker that runs Tick while a report is in progress; invalid when idle. */
		FTSTicker::FDelegateHandle TickerHandle;
		/** Notification that shows the report progress and the Cancel button. */
		Core::FReportProgressNotification ProgressNotification;
		/** Index in PendingAssets of the next master material to process. */
		int32 NextAssetIndex = 0;
		/** Stage the next Tick runs. */
		ERunnerStep Step = ERunnerStep::ShowNotification;
		/** When the report started, used to delay blocking work until the notification is visible. */
		double StepStartSeconds = 0.0;
		/** Materials processed in the current batch. */
		int32 MaterialsInBatch = 0;
		/** Assets loaded from disk in the current batch. */
		int32 BatchLoadedAssetCount = 0;
		/** When the current batch started, for the batch timing log. */
		double BatchStartSeconds = 0.0;
		/** Packages that were loaded before the current batch started. */
		TSet<UPackage*> PackagesBeforeBatch;
		/** Name of the last material processed, shown in the notification. */
		FString CurrentAssetName;
	};

	static TUniquePtr<FMaterialAnalyticsRunner> MaterialAnalyticsRunner;

	/** Starts the shared Material Analytics report runner. */
	void RunMaterialAnalyticsReport()
	{
		if (!MaterialAnalyticsRunner) MaterialAnalyticsRunner = MakeUnique<FMaterialAnalyticsRunner>();
		MaterialAnalyticsRunner->Start();
	}

	/** Stops the shared report runner and releases it. */
	void StopMaterialAnalyticsReport()
	{
		if (MaterialAnalyticsRunner)
		{
			MaterialAnalyticsRunner->Stop();
			MaterialAnalyticsRunner.Reset();
		}
	}
}
}

#undef LOCTEXT_NAMESPACE
