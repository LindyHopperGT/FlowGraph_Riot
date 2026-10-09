// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowAssetPropertyPolicy.h"

#include "FlowAsset.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	struct FFlowAssetPropertyRegistration
	{
		TSubclassOf<UFlowAsset> AssetClass;
		FName PropertyName;
		TSubclassOf<UObject> AllowedObjectClass;
	};

	TMap<FFlowAssetPropertyRegistrationHandle, FFlowAssetPropertyRegistration>& GetRegistrations()
	{
		static TMap<FFlowAssetPropertyRegistrationHandle, FFlowAssetPropertyRegistration> Registrations;
		return Registrations;
	}

	FFlowAssetPropertyRegistrationHandle& GetNextRegistrationHandle()
	{
		static FFlowAssetPropertyRegistrationHandle NextHandle = 1;
		return NextHandle;
	}

	const FFlowAssetPropertyRegistration* FindRegistration(const UClass* AssetClass, FName PropertyName)
	{
		for (const TPair<FFlowAssetPropertyRegistrationHandle, FFlowAssetPropertyRegistration>& Entry : GetRegistrations())
		{
			if (AssetClass && Entry.Value.PropertyName == PropertyName && AssetClass->IsChildOf(Entry.Value.AssetClass))
			{
				return &Entry.Value;
			}
		}
		return nullptr;
	}

	FObjectProperty* ResolveAllowedProperty(
		const UClass* AssetClass,
		FName PropertyName,
		const FFlowAssetPropertyRegistration& Registration)
	{
		FProperty* Property = AssetClass ? AssetClass->FindPropertyByName(PropertyName) : nullptr;
		FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
		if (!ObjectProperty || !ObjectProperty->PropertyClass ||
			!ObjectProperty->PropertyClass->IsChildOf(Registration.AllowedObjectClass))
		{
			return nullptr;
		}

		constexpr EPropertyFlags DisallowedFlags = CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated |
			CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance | CPF_InstancedReference |
			CPF_ContainsInstancedReference | CPF_PersistentInstance | CPF_AutoWeak | CPF_ExportObject | CPF_EditorOnly;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(DisallowedFlags))
		{
			return nullptr;
		}

		return ObjectProperty;
	}

	FString GetCanonicalValue(const FObjectPropertyBase& Property, const UObject* Container)
	{
		const UObject* Value = Property.GetObjectPropertyValue_InContainer(Container);
		return IsValid(Value) ? Value->GetPathName() : TEXT("None");
	}
}

FFlowAssetPropertyRegistrationHandle FFlowAssetPropertyPolicy::RegisterObjectReferenceProperty(
	TSubclassOf<UFlowAsset> AssetClass,
	FName PropertyName,
	TSubclassOf<UObject> AllowedObjectClass)
{
	if (!AssetClass || !AllowedObjectClass || PropertyName.IsNone())
	{
		return 0;
	}

	for (const TPair<FFlowAssetPropertyRegistrationHandle, FFlowAssetPropertyRegistration>& Entry : GetRegistrations())
	{
		const bool bClassesOverlap = AssetClass->IsChildOf(Entry.Value.AssetClass) ||
			Entry.Value.AssetClass->IsChildOf(AssetClass);
		if (Entry.Value.PropertyName == PropertyName && bClassesOverlap)
		{
			return 0;
		}
	}

	const FFlowAssetPropertyRegistration Candidate{AssetClass, PropertyName, AllowedObjectClass};
	if (!ResolveAllowedProperty(AssetClass, PropertyName, Candidate))
	{
		return 0;
	}

	FFlowAssetPropertyRegistrationHandle& NextHandle = GetNextRegistrationHandle();
	const FFlowAssetPropertyRegistrationHandle Handle = NextHandle++;
	GetRegistrations().Add(Handle, Candidate);
	return Handle;
}

void FFlowAssetPropertyPolicy::Unregister(FFlowAssetPropertyRegistrationHandle Handle)
{
	GetRegistrations().Remove(Handle);
}

void FFlowAssetPropertyPolicy::GetRegisteredObjectReferenceProperties(
	const UClass* AssetClass,
	TArray<FName>& OutPropertyNames)
{
	OutPropertyNames.Reset();
	for (const TPair<FFlowAssetPropertyRegistrationHandle, FFlowAssetPropertyRegistration>& Entry : GetRegistrations())
	{
		if (AssetClass && AssetClass->IsChildOf(Entry.Value.AssetClass) &&
			ResolveAllowedProperty(AssetClass, Entry.Value.PropertyName, Entry.Value))
		{
			OutPropertyNames.Add(Entry.Value.PropertyName);
		}
	}
	OutPropertyNames.Sort(FNameLexicalLess());
}

bool FFlowAssetPropertyPolicy::StageObjectReferenceProperty(
	const UClass* AssetClass,
	const UObject* CurrentAsset,
	FName PropertyName,
	const FString& Value,
	FFlowStagedAssetProperty& OutStagedProperty,
	FString& OutCode,
	FString& OutErrorMessage)
{
	OutCode.Reset();
	OutErrorMessage.Reset();
	OutStagedProperty = FFlowStagedAssetProperty();

	const FString PropertyNameString = PropertyName.ToString();
	if (PropertyNameString.Equals(TEXT("AssetClass"), ESearchCase::IgnoreCase) ||
		PropertyNameString.Equals(TEXT("bWorldBound"), ESearchCase::IgnoreCase) ||
		PropertyNameString.Equals(TEXT("ExpectedOwnerClass"), ESearchCase::IgnoreCase))
	{
		OutCode = TEXT("ConflictingAssetProperty");
		OutErrorMessage = FString::Printf(TEXT("'%s' is represented by a Courier document header and cannot also appear in assetProperties."), *PropertyName.ToString());
		return false;
	}

	const FFlowAssetPropertyRegistration* Registration = FindRegistration(AssetClass, PropertyName);
	if (!Registration)
	{
		OutCode = TEXT("UnsupportedAssetProperty");
		OutErrorMessage = FString::Printf(TEXT("Asset property '%s' is not registered for Flow asset class '%s'."),
			*PropertyName.ToString(), AssetClass ? *AssetClass->GetPathName() : TEXT("None"));
		return false;
	}

	FObjectProperty* Property = ResolveAllowedProperty(AssetClass, PropertyName, *Registration);
	if (!Property)
	{
		OutCode = TEXT("UnsupportedAssetProperty");
		OutErrorMessage = FString::Printf(TEXT("Registered asset property '%s' no longer has its approved editable object-reference type."),
			*PropertyName.ToString());
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FString CanonicalProposedValue;
	if (Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		CanonicalProposedValue = TEXT("None");
	}
	else
	{
		if (Value.IsEmpty() || Value != Value.TrimStartAndEnd() || !Value.StartsWith(TEXT("/")) || !Value.Contains(TEXT(".")))
		{
			OutCode = TEXT("InvalidAssetPropertyValue");
			OutErrorMessage = FString::Printf(TEXT("Asset property '%s' requires a full object path or 'None'."), *PropertyName.ToString());
			return false;
		}

		const FSoftObjectPath ObjectPath(Value);
		if (!ObjectPath.IsValid() || !ObjectPath.GetSubPathString().IsEmpty())
		{
			OutCode = TEXT("InvalidAssetPropertyValue");
			OutErrorMessage = FString::Printf(TEXT("Asset property '%s' requires a top-level asset object path."), *PropertyName.ToString());
			return false;
		}

		ResolvedObject = ObjectPath.TryLoad();
		if (!IsValid(ResolvedObject) || !ResolvedObject->GetClass()->IsChildOf(Registration->AllowedObjectClass))
		{
			OutCode = TEXT("UnresolvedAssetPropertyReference");
			OutErrorMessage = FString::Printf(TEXT("Asset property '%s' must resolve to a %s object; '%s' did not."),
				*PropertyName.ToString(), *Registration->AllowedObjectClass->GetName(), *Value);
			return false;
		}

		if (ResolvedObject->GetPathName() != Value)
		{
			OutCode = TEXT("InvalidAssetPropertyValue");
			OutErrorMessage = FString::Printf(TEXT("Asset property '%s' must use the referenced asset's full object path '%s'."),
				*PropertyName.ToString(), *ResolvedObject->GetPathName());
			return false;
		}
		CanonicalProposedValue = ResolvedObject->GetPathName();
	}

	const UObject* ComparisonAsset = CurrentAsset ? CurrentAsset : AssetClass->GetDefaultObject();
	const FString PreviousValue = GetCanonicalValue(*Property, ComparisonAsset);
	const UObject* PreviousObject = Property->GetObjectPropertyValue_InContainer(ComparisonAsset);

	OutStagedProperty.PropertyName = PropertyName;
	OutStagedProperty.PreviousValue = PreviousValue;
	OutStagedProperty.ProposedValue = CanonicalProposedValue;
	OutStagedProperty.Property = Property;
	OutStagedProperty.ResolvedObject.Reset(ResolvedObject);
	OutStagedProperty.bSupplied = true;
	OutStagedProperty.bChanged = PreviousObject != ResolvedObject;
	return true;
}

bool FFlowAssetPropertyPolicy::CopyRegisteredProperties(
	const UObject* SourceAsset,
	UObject* DestinationAsset,
	FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!IsValid(SourceAsset) || !IsValid(DestinationAsset) || !DestinationAsset->GetClass()->IsChildOf(SourceAsset->GetClass()))
	{
		OutErrorMessage = TEXT("Cannot copy registered Flow asset properties between incompatible assets.");
		return false;
	}

	TArray<FName> PropertyNames;
	GetRegisteredObjectReferenceProperties(SourceAsset->GetClass(), PropertyNames);
	for (const FName PropertyName : PropertyNames)
	{
		const FFlowAssetPropertyRegistration* Registration = FindRegistration(SourceAsset->GetClass(), PropertyName);
		FObjectProperty* SourceProperty = Registration ? ResolveAllowedProperty(SourceAsset->GetClass(), PropertyName, *Registration) : nullptr;
		FObjectProperty* DestinationProperty = Registration ? ResolveAllowedProperty(DestinationAsset->GetClass(), PropertyName, *Registration) : nullptr;
		if (!SourceProperty || !DestinationProperty)
		{
			OutErrorMessage = FString::Printf(TEXT("Registered asset property '%s' is no longer valid on the preview class."), *PropertyName.ToString());
			return false;
		}
		DestinationProperty->SetObjectPropertyValue_InContainer(
			DestinationAsset, SourceProperty->GetObjectPropertyValue_InContainer(SourceAsset));
	}
	return true;
}

bool FFlowAssetPropertyPolicy::SetStagedObjectReference(
	UObject* TargetAsset,
	const FFlowStagedAssetProperty& StagedProperty,
	bool bSendEditNotifications,
	FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!IsValid(TargetAsset) || !StagedProperty.bSupplied)
	{
		OutErrorMessage = TEXT("Cannot apply an invalid or omitted staged asset property.");
		return false;
	}

	const FFlowAssetPropertyRegistration* Registration = FindRegistration(TargetAsset->GetClass(), StagedProperty.PropertyName);
	FObjectProperty* Property = Registration
		? ResolveAllowedProperty(TargetAsset->GetClass(), StagedProperty.PropertyName, *Registration)
		: nullptr;
	if (!Property || Property != StagedProperty.Property)
	{
		OutErrorMessage = FString::Printf(TEXT("Asset property '%s' changed after preflight; refusing to apply a stale property reference."),
			*StagedProperty.PropertyName.ToString());
		return false;
	}

	UObject* const ProposedObject = StagedProperty.ResolvedObject.Get();
	if (ProposedObject && !ProposedObject->GetClass()->IsChildOf(Registration->AllowedObjectClass))
	{
		OutErrorMessage = FString::Printf(TEXT("Staged value for asset property '%s' no longer matches its registered object class."),
			*StagedProperty.PropertyName.ToString());
		return false;
	}
	if (!StagedProperty.bChanged)
	{
		return true;
	}
	if (bSendEditNotifications)
	{
		TargetAsset->PreEditChange(Property);
	}
	Property->SetObjectPropertyValue_InContainer(TargetAsset, ProposedObject);
	if (bSendEditNotifications)
	{
		FPropertyChangedEvent PropertyChangedEvent(Property, EPropertyChangeType::ValueSet);
		TargetAsset->PostEditChangeProperty(PropertyChangedEvent);
	}
	return true;
}
