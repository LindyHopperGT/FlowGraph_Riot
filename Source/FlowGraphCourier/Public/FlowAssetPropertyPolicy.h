// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/ObjectMacros.h"

class FProperty;
class UFlowAsset;
class UObject;
class UClass;

using FFlowAssetPropertyRegistrationHandle = uint64;

struct FLOWGRAPHCOURIER_API FFlowStagedAssetProperty
{
	FName PropertyName;
	FString PreviousValue;
	FString ProposedValue;
	FProperty* Property = nullptr;
	TStrongObjectPtr<UObject> ResolvedObject;
	bool bSupplied = false;
	bool bChanged = false;
};

/**
 * Explicit, deny-by-default policy for asset-level properties writable through Courier.
 * The initial value codec is limited to object references, represented by a full object path or "None".
 */
class FLOWGRAPHCOURIER_API FFlowAssetPropertyPolicy
{
public:
	static FFlowAssetPropertyRegistrationHandle RegisterObjectReferenceProperty(
		TSubclassOf<UFlowAsset> AssetClass,
		FName PropertyName,
		TSubclassOf<UObject> AllowedObjectClass);

	static void Unregister(FFlowAssetPropertyRegistrationHandle Handle);

	static void GetRegisteredObjectReferenceProperties(const UClass* AssetClass, TArray<FName>& OutPropertyNames);

	static bool StageObjectReferenceProperty(
		const UClass* AssetClass,
		const UObject* CurrentAsset,
		FName PropertyName,
		const FString& Value,
		FFlowStagedAssetProperty& OutStagedProperty,
		FString& OutCode,
		FString& OutErrorMessage);

	static bool CopyRegisteredProperties(const UObject* SourceAsset, UObject* DestinationAsset, FString& OutErrorMessage);

	static bool SetStagedObjectReference(
		UObject* TargetAsset,
		const FFlowStagedAssetProperty& StagedProperty,
		bool bSendEditNotifications,
		FString& OutErrorMessage);
};
