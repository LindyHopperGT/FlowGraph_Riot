// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphGlossaryExporter.h"
#include "FlowLogChannels.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowNodeBase.h"
#include "Nodes/FlowPin.h"
#include "Nodes/FlowNodeBlueprint.h"
#include "AddOns/FlowNodeAddOn.h"
#include "Nodes/FlowNodeAddOnBlueprint.h"
#include "Misc/FileHelper.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

bool UFlowGraphGlossaryExporter::ExportGlossaryToFile(const FString& OutputFilePath)
{
	FString GlossaryText = ExportGlossaryToString();
	
	if (FFileHelper::SaveStringToFile(GlossaryText, *OutputFilePath))
	{
		UE_LOG(LogFlow, Log, TEXT("FlowGraphGlossaryExporter: Successfully exported glossary to %s"), *OutputFilePath);
		return true;
	}
	else
	{
		UE_LOG(LogFlow, Error, TEXT("FlowGraphGlossaryExporter: Failed to write glossary to file %s"), *OutputFilePath);
		return false;
	}
}

FString UFlowGraphGlossaryExporter::ExportGlossaryToString()
{
	TArray<UClass*> NodeClasses;
	GatherAllNodeClasses(NodeClasses);
	
	FString Markdown;
	
	Markdown += TEXT("# FlowGraph Node Glossary\n\n");
	Markdown += TEXT("This document contains a comprehensive list of all available FlowNode types.\n");
	Markdown += TEXT("Use this reference when creating FlowAssets for missions.\n\n");
	
	// Add format documentation for LLMs
	Markdown +=
#include "FlowGraphGlossaryText.inl"
	;

	Markdown += FString::Printf(TEXT("**Total Nodes:** %d\n\n"), NodeClasses.Num());
	Markdown += TEXT("## Available Nodes\n\n");

	NodeClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});
	
	for (const UClass* NodeClass : NodeClasses)
	{
		ExportNodeToMarkdown(NodeClass, Markdown);
	}

	// AddOn section
	TArray<UClass*> AddOnClasses;
	GatherAllAddOnClasses(AddOnClasses);

	Markdown += TEXT("---\n\n");
	Markdown += FString::Printf(TEXT("**Total AddOns:** %d\n\n"), AddOnClasses.Num());
	Markdown += TEXT("## Available AddOns\n\n");
	Markdown += TEXT("AddOns extend node behavior and are attached in the node block using the `AddOn:` syntax described above.\n\n");

	AddOnClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});

	for (const UClass* AddOnClass : AddOnClasses)
	{
		ExportAddOnToMarkdown(AddOnClass, Markdown);
	}

	// Flow Asset Types section
	Markdown += TEXT("---\n\n");
	Markdown += TEXT("## Flow Asset Types\n\n");
	Markdown += TEXT("These are the FlowAsset subclasses that define different graph contexts (e.g. mission flow, action flow). Use this section to understand which nodes are valid in each context.\n\n");
	ExportFlowAssetTypesToMarkdown(Markdown, NodeClasses, AddOnClasses);

	// Keyword index — emit after all sections so the agent can query nodes by ecosystem keyword
	Markdown += TEXT("---\n\n");
	Markdown += TEXT("## Keyword Index\n\n");
	Markdown += TEXT("This index maps each keyword to every node and addon that carries it. ");
	Markdown += TEXT("Use a keyword to narrow the set of nodes relevant to a particular ecosystem ");
	Markdown += TEXT("(e.g. \"Encounter\", \"Blackboard\", \"SpawnActors\") before selecting nodes for a graph.\n\n");
	{
		TMap<FString, TArray<FString>> KeywordMap;
		GatherAllKeywords(NodeClasses, AddOnClasses, KeywordMap);

		// Sort keywords alphabetically for stable output
		TArray<FString> SortedKeywords;
		KeywordMap.GetKeys(SortedKeywords);
		SortedKeywords.Sort();

		for (const FString& Keyword : SortedKeywords)
		{
			const TArray<FString>& Names = KeywordMap[Keyword];
			Markdown += FString::Printf(TEXT("**%s:** %s\n\n"), *Keyword, *FString::Join(Names, TEXT(", ")));
		}
	}

	return Markdown;
}

FString UFlowGraphGlossaryExporter::GetNodeOrAddOnDetailsAsJson(const TArray<FString>& ClassNames)
{
	TArray<UClass*> NodeClasses;
	GatherAllNodeClasses(NodeClasses);

	TArray<UClass*> AddOnClasses;
	GatherAllAddOnClasses(AddOnClasses);

	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FString& RequestedName : ClassNames)
	{
		const UClass* FoundClass = nullptr;
		bool bIsAddOn = false;

		if (UClass* const* Found = NodeClasses.FindByPredicate([&RequestedName](const UClass* Class) { return Class->GetName() == RequestedName; }))
		{
			FoundClass = *Found;
		}
		else if (UClass* const* FoundAddOn = AddOnClasses.FindByPredicate([&RequestedName](const UClass* Class) { return Class->GetName() == RequestedName; }))
		{
			FoundClass = *FoundAddOn;
			bIsAddOn = true;
		}

		if (!FoundClass)
		{
			// Unknown class name - omitted from the result, not an error (caller can diff requested vs returned).
			continue;
		}

		TArray<FFlowGlossaryPropertyRow> Properties;
		GatherEditableProperties(FoundClass, Properties);

		TArray<FFlowGlossaryPinRow> InputPins;
		TArray<FFlowGlossaryPinRow> OutputPins;
		if (!bIsAddOn)
		{
			GatherPins(FoundClass, InputPins, OutputPins);
		}

		const FString ClassPath = FoundClass->GetPathName();
		const FString Signature = ComputeSignature(ClassPath, Properties, InputPins, OutputPins);

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("class_name"), RequestedName);
		Obj->SetStringField(TEXT("class_path"), ClassPath);
		Obj->SetBoolField(TEXT("is_addon"), bIsAddOn);
		Obj->SetStringField(TEXT("category"), bIsAddOn ? TEXT("") : GetNodeCategory(FoundClass));
		Obj->SetStringField(TEXT("description"), bIsAddOn ? GetAddOnDescription(FoundClass) : GetNodeDescription(FoundClass));
		Obj->SetStringField(TEXT("keywords"), GetNodeKeywords(FoundClass));
		Obj->SetStringField(TEXT("signature"), Signature);

		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		for (const FFlowGlossaryPropertyRow& Row : Properties)
		{
			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), Row.Name);
			PropObj->SetStringField(TEXT("type"), Row.Type);
			PropObj->SetStringField(TEXT("pin_binding"), Row.PinBinding);
			PropObj->SetStringField(TEXT("description"), Row.Description);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(PropObj));
		}
		Obj->SetArrayField(TEXT("properties"), PropertiesJson);

		auto PinRowsToJson = [](const TArray<FFlowGlossaryPinRow>& Rows)
		{
			TArray<TSharedPtr<FJsonValue>> PinsJson;
			for (const FFlowGlossaryPinRow& Row : Rows)
			{
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Row.Name);
				PinObj->SetStringField(TEXT("type"), Row.Type);
				PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			return PinsJson;
		};
		Obj->SetArrayField(TEXT("input_pins"), PinRowsToJson(InputPins));
		Obj->SetArrayField(TEXT("output_pins"), PinRowsToJson(OutputPins));

		ResultArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString OutputJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
	FJsonSerializer::Serialize(ResultArray, Writer);
	return OutputJson;
}

void UFlowGraphGlossaryExporter::GatherAllNodeClasses(TArray<UClass*>& OutNodeClasses)
{
	OutNodeClasses.Reset();
	
	TArray<UClass*> NativeFlowNodes;
	GetDerivedClasses(UFlowNode::StaticClass(), NativeFlowNodes);
	
	for (UClass* Class : NativeFlowNodes)
	{
		if (Class->ClassGeneratedBy != nullptr)
		{
			continue;
		}
		
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		
		if (Class->GetName().StartsWith(TEXT("SKEL_")) || 
			Class->GetName().StartsWith(TEXT("REINST_")) ||
			Class->GetName().StartsWith(TEXT("TRASHCLASS_")))
		{
			continue;
		}
		
		const UFlowNode* DefaultNode = Class->GetDefaultObject<UFlowNode>();
		if (DefaultNode)
		{
			FString Category = DefaultNode->GetNodeCategory();
			if (Category.Contains(TEXT("Deprecated"), ESearchCase::IgnoreCase) || DefaultNode->GetClass()->GetBoolMetaData(TEXT("DeprecatedNode")))
			{
				continue;
			}
		}
		
		OutNodeClasses.Add(Class);
	}
	
	FARFilter Filter;
	Filter.ClassPaths.Add(UFlowNodeBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	
	TArray<FAssetData> FoundAssets;
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(AssetRegistryConstants::ModuleName);
	AssetRegistryModule.Get().GetAssets(Filter, FoundAssets);
	
	for (const FAssetData& AssetData : FoundAssets)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
		{
			UClass* GeneratedClass = Blueprint->GeneratedClass;
			if (GeneratedClass && GeneratedClass->IsChildOf(UFlowNode::StaticClass()))
			{
				if (!GeneratedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					const UFlowNode* DefaultNode = GeneratedClass->GetDefaultObject<UFlowNode>();
					if (DefaultNode)
					{
						FString Category = DefaultNode->GetNodeCategory();
						if (Category.Contains(TEXT("Deprecated"), ESearchCase::IgnoreCase) || DefaultNode->GetClass()->GetBoolMetaData(TEXT("DeprecatedNode")))
						{
							continue;
						}
					}
					OutNodeClasses.Add(GeneratedClass);
				}
			}
		}
	}
}

void UFlowGraphGlossaryExporter::ExportNodeToMarkdown(const UClass* NodeClass, FString& OutMarkdown)
{
	if (!NodeClass)
	{
		return;
	}
	
	FString NodeName = NodeClass->GetName();
	NodeName.RemoveFromStart(TEXT("FlowNode_"));
	
	OutMarkdown += FString::Printf(TEXT("## %s\n\n"), *NodeName);
	
	OutMarkdown += FString::Printf(TEXT("**Class:** `%s`\n\n"), *NodeClass->GetPathName());
	
	FString Category = GetNodeCategory(NodeClass);
	if (!Category.IsEmpty())
	{
		OutMarkdown += FString::Printf(TEXT("**Category:** %s\n\n"), *Category);
	}
	
	FString Description = GetNodeDescription(NodeClass);
	if (!Description.IsEmpty())
	{
		OutMarkdown += FString::Printf(TEXT("**Description:** %s\n\n"), *Description);
	}

	const FString Keywords = GetNodeKeywords(NodeClass);
	if (!Keywords.IsEmpty())
	{
		OutMarkdown += FString::Printf(TEXT("**Keywords:** %s\n\n"), *Keywords);
	}

	bool bCanAddInputs = CanNodeAddUserInputPins(NodeClass);
	bool bCanAddOutputs = CanNodeAddUserOutputPins(NodeClass);
	
	if (bCanAddInputs || bCanAddOutputs)
	{
		OutMarkdown += TEXT("**Dynamic Pins:**\n");
		if (bCanAddInputs)
		{
			OutMarkdown += TEXT("- Supports user-added input pins\n");
		}
		if (bCanAddOutputs)
		{
			OutMarkdown += TEXT("- Supports user-added output pins\n");
		}
		OutMarkdown += TEXT("\n");
	}
	
	ExportPropertiesToMarkdown(NodeClass, OutMarkdown);
	ExportPinsToMarkdown(NodeClass, OutMarkdown);
	
	OutMarkdown += TEXT("---\n\n");
}

FString UFlowGraphGlossaryExporter::GetNodeDescription(const UClass* NodeClass)
{
	if (!NodeClass)
	{
		return TEXT("");
	}
	
#if WITH_EDITOR
	const UFlowNode* DefaultNode = NodeClass->GetDefaultObject<UFlowNode>();
	if (DefaultNode)
	{
		FText Tooltip = DefaultNode->GetNodeToolTip();
		if (!Tooltip.IsEmpty())
		{
			return Tooltip.ToString();
		}
	}
	
	FText ClassTooltip = NodeClass->GetToolTipText();
	if (!ClassTooltip.IsEmpty())
	{
		return ClassTooltip.ToString();
	}
#endif
	
	return TEXT("");
}

FString UFlowGraphGlossaryExporter::GetNodeCategory(const UClass* NodeClass)
{
	if (!NodeClass)
	{
		return TEXT("");
	}
	
	const UFlowNode* DefaultNode = NodeClass->GetDefaultObject<UFlowNode>();
	if (DefaultNode)
	{
		return DefaultNode->GetNodeCategory();
	}
	
	return TEXT("");
}

void UFlowGraphGlossaryExporter::GatherEditableProperties(const UClass* NodeOrAddOnClass, TArray<FFlowGlossaryPropertyRow>& OutRows)
{
	OutRows.Reset();

	if (!NodeOrAddOnClass)
	{
		return;
	}

	for (TFieldIterator<FProperty> PropIt(NodeOrAddOnClass); PropIt; ++PropIt)
	{
		const FProperty* Property = *PropIt;

		if (!Property)
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
		{
			continue;
		}

		if (Property->GetOwnerClass() == UFlowNode::StaticClass() ||
			Property->GetOwnerClass() == UFlowNodeBase::StaticClass())
		{
			continue;
		}

		bool bIsEditable = Property->HasAnyPropertyFlags(CPF_Edit);
		bool bIsBlueprintReadOnly = Property->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
									 !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);

		if (!bIsEditable && !bIsBlueprintReadOnly)
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			continue;
		}

		FFlowGlossaryPropertyRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Name = Property->GetName();
		Row.Type = GetPropertyTypeName(Property);
		Row.PinBinding = GetPropertyPinBinding(Property);
		Row.Description = GetPropertyTooltip(Property);
	}
}

void UFlowGraphGlossaryExporter::GatherPins(const UClass* NodeClass, TArray<FFlowGlossaryPinRow>& OutInputPins, TArray<FFlowGlossaryPinRow>& OutOutputPins)
{
	OutInputPins.Reset();
	OutOutputPins.Reset();

	const UFlowNode* DefaultNode = NodeClass ? NodeClass->GetDefaultObject<UFlowNode>() : nullptr;
	if (!DefaultNode)
	{
		// AddOns (and anything else that isn't a UFlowNode) simply have no pin list of their own.
		return;
	}

	for (const FFlowPin& Pin : DefaultNode->GetInputPins())
	{
		FFlowGlossaryPinRow& Row = OutInputPins.AddDefaulted_GetRef();
		Row.Name = Pin.PinName.ToString();
		Row.Type = GetPinTypeName(Pin);
	}

	for (const FFlowPin& Pin : DefaultNode->GetOutputPins())
	{
		FFlowGlossaryPinRow& Row = OutOutputPins.AddDefaulted_GetRef();
		Row.Name = Pin.PinName.ToString();
		Row.Type = GetPinTypeName(Pin);
	}
}

FString UFlowGraphGlossaryExporter::ComputeSignature(
	const FString& ClassPath,
	const TArray<FFlowGlossaryPropertyRow>& Properties,
	const TArray<FFlowGlossaryPinRow>& InputPins,
	const TArray<FFlowGlossaryPinRow>& OutputPins)
{
	TArray<FString> PropertyStrings;
	for (const FFlowGlossaryPropertyRow& Row : Properties)
	{
		PropertyStrings.Add(FString::Printf(TEXT("%s:%s:%s"), *Row.Name, *Row.Type, *Row.PinBinding));
	}
	PropertyStrings.Sort();

	TArray<FString> InputPinNames;
	for (const FFlowGlossaryPinRow& Row : InputPins)
	{
		InputPinNames.Add(Row.Name);
	}
	InputPinNames.Sort();

	TArray<FString> OutputPinNames;
	for (const FFlowGlossaryPinRow& Row : OutputPins)
	{
		OutputPinNames.Add(Row.Name);
	}
	OutputPinNames.Sort();

	const FString Canonical = ClassPath
		+ TEXT("|") + FString::Join(PropertyStrings, TEXT("|"))
		+ TEXT("|") + FString::Join(InputPinNames, TEXT("|"))
		+ TEXT("|") + FString::Join(OutputPinNames, TEXT("|"));

	// NOTE: FPlatformMisc::GetSHA256Signature is NOT implemented on this platform - it hits the
	// FGenericPlatformMisc fallback, which asserts and zeroes the output (confirmed live, see
	// FLOW_CODEV_AGENT_PLAN.md Section 10 retro). FSHA1::HashBuffer is a pure-software, platform-
	// independent implementation with no such gap, so the signature algorithm is SHA-1 (not
	// SHA-256 as GARDENING.md originally specified) - GARDENING.md Section 5 must be updated to
	// match, and any already-written `signature:` values recomputed against this function.
	const FTCHARToUTF8 Utf8Canonical(*Canonical);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8Canonical.Get(), Utf8Canonical.Length(), Digest);

	FString HexDigest;
	for (int32 Idx = 0; Idx < FSHA1::DigestSize; ++Idx)
	{
		HexDigest += FString::Printf(TEXT("%02x"), Digest[Idx]);
	}

	return HexDigest.Left(12);
}

void UFlowGraphGlossaryExporter::ExportPropertiesToMarkdown(const UClass* NodeClass, FString& OutMarkdown)
{
	if (!NodeClass)
	{
		return;
	}

	TArray<FFlowGlossaryPropertyRow> Rows;
	GatherEditableProperties(NodeClass, Rows);

	if (Rows.Num() == 0)
	{
		OutMarkdown += TEXT("### Properties\n\n");
		OutMarkdown += TEXT("*No editable properties*\n\n");
		return;
	}

	OutMarkdown += TEXT("### Properties\n\n");
	OutMarkdown += TEXT("| Property | Type | Pin Binding | Description |\n");
	OutMarkdown += TEXT("|----------|------|-------------|-------------|\n");

	for (const FFlowGlossaryPropertyRow& Row : Rows)
	{
		FString Description = Row.Description;
		Description.ReplaceInline(TEXT("|"), TEXT("\\|"));
		Description.ReplaceInline(TEXT("\n"), TEXT(" "));
		Description.ReplaceInline(TEXT("\r"), TEXT(""));

		OutMarkdown += FString::Printf(TEXT("| `%s` | %s | %s | %s |\n"),
			*Row.Name, *Row.Type, *Row.PinBinding, *Description);
	}

	OutMarkdown += TEXT("\n");
}

void UFlowGraphGlossaryExporter::ExportPinsToMarkdown(const UClass* NodeClass, FString& OutMarkdown)
{
	if (!NodeClass)
	{
		return;
	}
	
	const UFlowNode* DefaultNode = NodeClass->GetDefaultObject<UFlowNode>();
	if (!DefaultNode)
	{
		return;
	}
	
	const TArray<FFlowPin>& InputPins = DefaultNode->GetInputPins();
	const TArray<FFlowPin>& OutputPins = DefaultNode->GetOutputPins();
	
	OutMarkdown += TEXT("### Input Pins\n\n");
	if (InputPins.Num() == 0)
	{
		OutMarkdown += TEXT("*No input pins*\n\n");
	}
	else
	{
		OutMarkdown += TEXT("| Pin Name | Type |\n");
		OutMarkdown += TEXT("|----------|------|\n");
		
		for (const FFlowPin& Pin : InputPins)
		{
			FString PinType = GetPinTypeName(Pin);
			OutMarkdown += FString::Printf(TEXT("| `%s` | %s |\n"), 
				*Pin.PinName.ToString(), *PinType);
		}
		OutMarkdown += TEXT("\n");
	}
	
	OutMarkdown += TEXT("### Output Pins\n\n");
	if (OutputPins.Num() == 0)
	{
		OutMarkdown += TEXT("*No output pins*\n\n");
	}
	else
	{
		OutMarkdown += TEXT("| Pin Name | Type |\n");
		OutMarkdown += TEXT("|----------|------|\n");
		
		for (const FFlowPin& Pin : OutputPins)
		{
			FString PinType = GetPinTypeName(Pin);
			OutMarkdown += FString::Printf(TEXT("| `%s` | %s |\n"), 
				*Pin.PinName.ToString(), *PinType);
		}
		OutMarkdown += TEXT("\n");
	}
}

FString UFlowGraphGlossaryExporter::GetPropertyTypeName(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("Unknown");
	}
	
	if (CastField<FBoolProperty>(Property))
	{
		return TEXT("Boolean");
	}
	else if (CastField<FIntProperty>(Property))
	{
		return TEXT("Integer");
	}
	else if (CastField<FInt64Property>(Property))
	{
		return TEXT("Integer64");
	}
	else if (CastField<FFloatProperty>(Property))
	{
		return TEXT("Float");
	}
	else if (CastField<FDoubleProperty>(Property))
	{
		return TEXT("Double");
	}
	else if (CastField<FStrProperty>(Property))
	{
		return TEXT("String");
	}
	else if (CastField<FNameProperty>(Property))
	{
		return TEXT("Name");
	}
	else if (CastField<FTextProperty>(Property))
	{
		return TEXT("Text");
	}
	else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		if (const UEnum* Enum = EnumProp->GetEnum())
		{
			return FString::Printf(TEXT("Enum (%s)"), *Enum->GetName());
		}
		return TEXT("Enum");
	}
	else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (ByteProp->Enum)
		{
			return FString::Printf(TEXT("Enum (%s)"), *ByteProp->Enum->GetName());
		}
		return TEXT("Byte");
	}
	else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		if (ObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("Object (%s)"), *ObjProp->PropertyClass->GetName());
		}
		return TEXT("Object");
	}
	else if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		if (SoftObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("Soft Object (%s)"), *SoftObjProp->PropertyClass->GetName());
		}
		return TEXT("Soft Object");
	}
	else if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		if (ClassProp->MetaClass)
		{
			return FString::Printf(TEXT("Class (%s)"), *ClassProp->MetaClass->GetName());
		}
		return TEXT("Class");
	}
	else if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
	{
		if (SoftClassProp->MetaClass)
		{
			return FString::Printf(TEXT("Soft Class (%s)"), *SoftClassProp->MetaClass->GetName());
		}
		return TEXT("Soft Class");
	}
	else if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct)
		{
			return FString::Printf(TEXT("Struct (%s)"), *StructProp->Struct->GetName());
		}
		return TEXT("Struct");
	}
	else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		FString InnerType = GetPropertyTypeName(ArrayProp->Inner);
		return FString::Printf(TEXT("Array<%s>"), *InnerType);
	}
	else if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		FString ElementType = GetPropertyTypeName(SetProp->ElementProp);
		return FString::Printf(TEXT("Set<%s>"), *ElementType);
	}
	else if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		FString KeyType = GetPropertyTypeName(MapProp->KeyProp);
		FString ValueType = GetPropertyTypeName(MapProp->ValueProp);
		return FString::Printf(TEXT("Map<%s, %s>"), *KeyType, *ValueType);
	}
	
	return Property->GetCPPType();
}

FString UFlowGraphGlossaryExporter::GetPropertyTooltip(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("");
	}
	
#if WITH_EDITOR
	FText Tooltip = Property->GetToolTipText();
	if (!Tooltip.IsEmpty())
	{
		return Tooltip.ToString();
	}
#endif
	
	return TEXT("");
}

FString UFlowGraphGlossaryExporter::GetPropertyPinBinding(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("-");
	}
	
#if WITH_EDITOR
	bool bIsOutputPin = Property->HasMetaData(FFlowPin::MetadataKey_SourceForOutputFlowPin);
	bool bIsInputPin = Property->HasMetaData(FFlowPin::MetadataKey_DefaultForInputFlowPin);
	bool bHasFlowPinType = Property->HasMetaData(FFlowPin::MetadataKey_FlowPinType);
	
	if (bIsOutputPin)
	{
		FString PinName = Property->GetMetaData(FFlowPin::MetadataKey_SourceForOutputFlowPin);
		if (PinName.IsEmpty())
		{
			PinName = Property->GetDisplayNameText().ToString();
		}
		return FString::Printf(TEXT("Output Pin: `%s`"), *PinName);
	}
	else if (bIsInputPin)
	{
		FString PinName = Property->GetMetaData(FFlowPin::MetadataKey_DefaultForInputFlowPin);
		if (PinName.IsEmpty())
		{
			PinName = Property->GetDisplayNameText().ToString();
		}
		return FString::Printf(TEXT("Input Pin (default): `%s`"), *PinName);
	}
	else if (bHasFlowPinType)
	{
		FString PinType = Property->GetMetaData(FFlowPin::MetadataKey_FlowPinType);
		return FString::Printf(TEXT("Data Pin (%s)"), *PinType);
	}
	
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct)
		{
			if (StructProp->Struct->HasMetaData(FFlowPin::MetadataKey_FlowPinType))
			{
				FString PinType = StructProp->Struct->GetMetaData(FFlowPin::MetadataKey_FlowPinType);
				return FString::Printf(TEXT("Data Pin (%s)"), *PinType);
			}
			
			FString StructName = StructProp->Struct->GetName();
			if (StructName.StartsWith(TEXT("FlowDataPinValue_")) || 
				StructName.StartsWith(TEXT("FlowDataPinOutputProperty_")) ||
				StructName.StartsWith(TEXT("FlowDataPinInputProperty_")))
			{
				FString PinType = StructName;
				PinType.RemoveFromStart(TEXT("FlowDataPinValue_"));
				PinType.RemoveFromStart(TEXT("FlowDataPinOutputProperty_"));
				PinType.RemoveFromStart(TEXT("FlowDataPinInputProperty_"));
				return FString::Printf(TEXT("Data Pin (%s)"), *PinType);
			}
		}
	}
#endif
	
	return TEXT("-");
}

FString UFlowGraphGlossaryExporter::GetPinTypeName(const FFlowPin& Pin)
{
	const FFlowPinTypeName& PinTypeName = Pin.GetPinTypeName();
	FString TypeStr = PinTypeName.ToString();
	
	if (TypeStr.IsEmpty() || TypeStr == TEXT("exec") || TypeStr == TEXT("Exec"))
	{
		return TEXT("Exec");
	}
	
	return TypeStr;
}

bool UFlowGraphGlossaryExporter::CanNodeAddUserInputPins(const UClass* NodeClass)
{
	if (!NodeClass)
	{
		return false;
	}
	
#if WITH_EDITOR
	const UFlowNode* DefaultNode = NodeClass->GetDefaultObject<UFlowNode>();
	if (DefaultNode)
	{
		return DefaultNode->CanUserAddInput();
	}
#endif
	
	return false;
}

bool UFlowGraphGlossaryExporter::CanNodeAddUserOutputPins(const UClass* NodeClass)
{
	if (!NodeClass)
	{
		return false;
	}
	
#if WITH_EDITOR
	const UFlowNode* DefaultNode = NodeClass->GetDefaultObject<UFlowNode>();
	if (DefaultNode)
	{
		return DefaultNode->CanUserAddOutput();
	}
#endif
	
	return false;
}

void UFlowGraphGlossaryExporter::GatherAllAddOnClasses(TArray<UClass*>& OutAddOnClasses)
{
	OutAddOnClasses.Reset();

	// Native C++ addon classes
	TArray<UClass*> NativeAddOns;
	GetDerivedClasses(UFlowNodeAddOn::StaticClass(), NativeAddOns);

	for (UClass* Class : NativeAddOns)
	{
		if (Class->ClassGeneratedBy != nullptr)
		{
			continue;
		}

		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		if (Class->GetName().StartsWith(TEXT("SKEL_")) ||
			Class->GetName().StartsWith(TEXT("REINST_")) ||
			Class->GetName().StartsWith(TEXT("TRASHCLASS_")))
		{
			continue;
		}

		OutAddOnClasses.Add(Class);
	}

	// Blueprint addon classes via asset registry
	FARFilter Filter;
	Filter.ClassPaths.Add(UFlowNodeAddOnBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> FoundAssets;
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(AssetRegistryConstants::ModuleName);
	AssetRegistryModule.Get().GetAssets(Filter, FoundAssets);

	for (const FAssetData& AssetData : FoundAssets)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
		{
			UClass* GeneratedClass = Blueprint->GeneratedClass;
			if (GeneratedClass && GeneratedClass->IsChildOf(UFlowNodeAddOn::StaticClass()))
			{
				if (!GeneratedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					OutAddOnClasses.Add(GeneratedClass);
				}
			}
		}
	}
}

void UFlowGraphGlossaryExporter::ExportAddOnToMarkdown(const UClass* AddOnClass, FString& OutMarkdown)
{
	if (!AddOnClass)
	{
		return;
	}

	FString AddOnName = AddOnClass->GetName();
	// Strip generated class suffix for Blueprint classes
	AddOnName.RemoveFromEnd(TEXT("_C"));

	OutMarkdown += FString::Printf(TEXT("## %s\n\n"), *AddOnName);
	OutMarkdown += FString::Printf(TEXT("**Class:** `%s`\n\n"), *AddOnClass->GetPathName());

	FString Description = GetAddOnDescription(AddOnClass);
	if (!Description.IsEmpty())
	{
		OutMarkdown += FString::Printf(TEXT("**Description:** %s\n\n"), *Description);
	}

	const FString Keywords = GetNodeKeywords(AddOnClass);
	if (!Keywords.IsEmpty())
	{
		OutMarkdown += FString::Printf(TEXT("**Keywords:** %s\n\n"), *Keywords);
	}

	ExportAddOnPropertiesToMarkdown(AddOnClass, OutMarkdown);

	OutMarkdown += TEXT("---\n\n");
}

void UFlowGraphGlossaryExporter::ExportAddOnPropertiesToMarkdown(const UClass* AddOnClass, FString& OutMarkdown)
{
	if (!AddOnClass)
	{
		return;
	}

	TArray<const FProperty*> EditableProperties;

	for (TFieldIterator<FProperty> PropIt(AddOnClass); PropIt; ++PropIt)
	{
		const FProperty* Property = *PropIt;

		if (!Property)
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
		{
			continue;
		}

		// Skip base-class infrastructure properties
		UClass* OwnerClass = Property->GetOwnerClass();
		if (OwnerClass == UFlowNodeBase::StaticClass() || OwnerClass == UFlowNodeAddOn::StaticClass())
		{
			continue;
		}

		bool bIsEditable = Property->HasAnyPropertyFlags(CPF_Edit);
		bool bIsBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
								   !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);

		if (!bIsEditable && !bIsBlueprintVisible)
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
		{
			continue;
		}

		EditableProperties.Add(Property);
	}

	if (EditableProperties.Num() == 0)
	{
		OutMarkdown += TEXT("### Properties\n\n");
		OutMarkdown += TEXT("*No editable properties*\n\n");
		return;
	}

	OutMarkdown += TEXT("### Properties\n\n");
	OutMarkdown += TEXT("| Property | Type | Description |\n");
	OutMarkdown += TEXT("|----------|------|-------------|\n");

	for (const FProperty* Property : EditableProperties)
	{
		FString PropName = Property->GetName();
		FString PropType = GetPropertyTypeName(Property);
		FString PropTooltip = GetPropertyTooltip(Property);

		PropTooltip.ReplaceInline(TEXT("|"), TEXT("\\|"));
		PropTooltip.ReplaceInline(TEXT("\n"), TEXT(" "));
		PropTooltip.ReplaceInline(TEXT("\r"), TEXT(""));

		OutMarkdown += FString::Printf(TEXT("| `%s` | %s | %s |\n"),
			*PropName, *PropType, *PropTooltip);
	}

	OutMarkdown += TEXT("\n");
}

FString UFlowGraphGlossaryExporter::GetAddOnDescription(const UClass* AddOnClass)
{
	if (!AddOnClass)
	{
		return TEXT("");
	}

#if WITH_EDITOR
	FText ClassTooltip = AddOnClass->GetToolTipText();
	if (!ClassTooltip.IsEmpty())
	{
		return ClassTooltip.ToString();
	}
#endif

	return TEXT("");
}

FString UFlowGraphGlossaryExporter::GetNodeKeywords(const UClass* NodeClass)
{
	if (!NodeClass)
	{
		return TEXT("");
	}

#if WITH_EDITOR
	static const FName NAME_Keywords(TEXT("Keywords"));
	if (NodeClass->HasMetaData(NAME_Keywords))
	{
		return NodeClass->GetMetaData(NAME_Keywords);
	}
#endif

	return TEXT("");
}

void UFlowGraphGlossaryExporter::GatherAllKeywords(
	const TArray<UClass*>& NodeClasses,
	const TArray<UClass*>& AddOnClasses,
	TMap<FString, TArray<FString>>& OutKeywordMap)
{
	OutKeywordMap.Reset();

	auto AccumulateClass = [&OutKeywordMap](const UClass* Class)
	{
		if (!Class)
		{
			return;
		}

		const FString RawKeywords = GetNodeKeywords(Class);
		if (RawKeywords.IsEmpty())
		{
			return;
		}

		// Determine the display name: prefer DisplayName meta, else strip common prefixes
		FString DisplayName;
#if WITH_EDITOR
		static const FName NAME_DisplayName(TEXT("DisplayName"));
		if (Class->HasMetaData(NAME_DisplayName))
		{
			DisplayName = Class->GetMetaData(NAME_DisplayName);
		}
#endif
		if (DisplayName.IsEmpty())
		{
			DisplayName = Class->GetName();
			DisplayName.RemoveFromStart(TEXT("FlowNode_"));
			DisplayName.RemoveFromStart(TEXT("FlowNodeAddOn_"));
			DisplayName.RemoveFromEnd(TEXT("_C"));
		}

		// Split on whitespace and commas; trim and lower-case each token
		TArray<FString> Tokens;
		RawKeywords.ParseIntoArray(Tokens, TEXT(" "), /*bCullEmpty=*/true);
		for (FString& Token : Tokens)
		{
			Token = Token.TrimStartAndEnd().Replace(TEXT(","), TEXT("")).ToLower();
			if (!Token.IsEmpty())
			{
				TArray<FString>& Entry = OutKeywordMap.FindOrAdd(Token);
				Entry.AddUnique(DisplayName);
			}
		}
	};

	for (const UClass* Class : NodeClasses)
	{
		AccumulateClass(Class);
	}
	for (const UClass* Class : AddOnClasses)
	{
		AccumulateClass(Class);
	}

	// Sort display-name lists within each keyword entry for stable output
	for (auto& Pair : OutKeywordMap)
	{
		Pair.Value.Sort();
	}
}

void UFlowGraphGlossaryExporter::GatherAllFlowAssetClasses(TArray<UClass*>& OutAssetClasses)
{
	OutAssetClasses.Reset();

	TArray<UClass*> Derived;
	GetDerivedClasses(UFlowAsset::StaticClass(), Derived, /*bRecursive=*/true);

	for (UClass* Class : Derived)
	{
		if (Class->ClassGeneratedBy != nullptr)
		{
			continue;
		}

		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		if (Class->GetName().StartsWith(TEXT("SKEL_")) ||
			Class->GetName().StartsWith(TEXT("REINST_")) ||
			Class->GetName().StartsWith(TEXT("TRASHCLASS_")))
		{
			continue;
		}

		OutAssetClasses.Add(Class);
	}

	OutAssetClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});
}

void UFlowGraphGlossaryExporter::ExportFlowAssetTypesToMarkdown(
	FString& OutMarkdown,
	const TArray<UClass*>& NodeClasses,
	const TArray<UClass*>& AddOnClasses)
{
#if WITH_EDITOR
	TArray<UClass*> AssetClasses;
	GatherAllFlowAssetClasses(AssetClasses);

	for (const UClass* AssetClass : AssetClasses)
	{
		FString ClassName = AssetClass->GetName();

		OutMarkdown += FString::Printf(TEXT("### %s\n\n"), *ClassName);
		OutMarkdown += FString::Printf(TEXT("**Class:** `%s`\n\n"), *AssetClass->GetPathName());

		const FText ClassTooltip = AssetClass->GetToolTipText();
		if (!ClassTooltip.IsEmpty())
		{
			OutMarkdown += FString::Printf(TEXT("**Description:** %s\n\n"), *ClassTooltip.ToString());
		}

		const FString Keywords = GetNodeKeywords(AssetClass);
		if (!Keywords.IsEmpty())
		{
			OutMarkdown += FString::Printf(TEXT("**Keywords:** %s\n\n"), *Keywords);
		}

		// Resolve the allowed-node/allowed-addon closure via the asset's own CDO, reusing
		// the exact same logic the editor uses to validate/filter the node palette
		// (UFlowAsset::IsNodeOrAddOnClassAllowed), instead of re-deriving Allowed/DeniedNodeClasses
		// resolution here. This lets agents filter the catalog to a given asset type's palette.
		const UFlowAsset* AssetCDO = AssetClass->GetDefaultObject<UFlowAsset>();
		if (AssetCDO)
		{
			TArray<FString> AllowedNodeNames;
			for (const UClass* NodeClass : NodeClasses)
			{
				if (AssetCDO->IsNodeOrAddOnClassAllowed(NodeClass))
				{
					AllowedNodeNames.Add(NodeClass->GetName());
				}
			}

			TArray<FString> AllowedAddOnNames;
			for (const UClass* AddOnClass : AddOnClasses)
			{
				if (AssetCDO->IsNodeOrAddOnClassAllowed(AddOnClass))
				{
					AllowedAddOnNames.Add(AddOnClass->GetName());
				}
			}

			OutMarkdown += FString::Printf(TEXT("**Allowed Nodes (%d of %d):** %s\n\n"),
				AllowedNodeNames.Num(), NodeClasses.Num(),
				*FString::Join(AllowedNodeNames, TEXT(", ")));
			OutMarkdown += FString::Printf(TEXT("**Allowed AddOns (%d of %d):** %s\n\n"),
				AllowedAddOnNames.Num(), AddOnClasses.Num(),
				*FString::Join(AllowedAddOnNames, TEXT(", ")));
		}

		OutMarkdown += TEXT("\n");
	}
#endif
}
