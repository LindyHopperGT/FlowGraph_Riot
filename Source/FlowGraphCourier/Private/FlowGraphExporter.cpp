// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphExporter.h"
#include "FlowLogChannels.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "AddOns/FlowNodeAddOn.h"
#include "Nodes/FlowNodeBase.h"
#include "Nodes/FlowPin.h"
#include "Misc/FileHelper.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

bool UFlowGraphExporter::ExportFlowGraphToText(UFlowAsset* FlowAsset, const FString& OutputFilePath)
{
	if (!FlowAsset)
	{
		UE_LOG(LogFlow, Error, TEXT("FlowGraphExporter: FlowAsset is null"));
		return false;
	}

	const FString ExportText = ExportFlowGraphToString(FlowAsset);

	// TODO (gtaylor) Downgrade these logs to Verbose after the import/export mechanism has stabilized.
	UE_LOG(LogFlow, Log, TEXT("ExportFlowGraphToText:\n%s"), *ExportText);

	if (FFileHelper::SaveStringToFile(ExportText, *OutputFilePath))
	{
		UE_LOG(LogFlow, Log, TEXT("FlowGraphExporter: Successfully exported to %s"), *OutputFilePath);
		return true;
	}
	else
	{
		UE_LOG(LogFlow, Error, TEXT("FlowGraphExporter: Failed to write to file %s"), *OutputFilePath);
		return false;
	}
}

FString UFlowGraphExporter::ExportFlowGraphToString(UFlowAsset* FlowAsset)
{
	if (!FlowAsset)
	{
		return TEXT("");
	}

	FString ExportText;
	
	ExportHeader(FlowAsset, ExportText);
	ExportNodes(FlowAsset, ExportText);
	ExportConnections(FlowAsset, ExportText);
	
	return ExportText;
}

void UFlowGraphExporter::ExportHeader(const UFlowAsset* FlowAsset, FString& OutText)
{
	OutText += TEXT("FlowGraphExport v1.0\n");
	OutText += FString::Printf(TEXT("WorldBound: %s\n"), FlowAsset->bWorldBound ? TEXT("true") : TEXT("false"));
	
	UClass* ExpectedOwnerClass = FlowAsset->GetExpectedOwnerClass();
	OutText += FString::Printf(TEXT("ExpectedOwnerClass: %s\n"), 
		ExpectedOwnerClass ? *ExpectedOwnerClass->GetName() : TEXT("None"));
	
	OutText += TEXT("---\n\n");
}

void UFlowGraphExporter::ExportNodes(const UFlowAsset* FlowAsset, FString& OutText)
{
	OutText += TEXT("[NODES]\n");
	
	const TMap<FGuid, UFlowNode*>& Nodes = FlowAsset->GetNodes();
	
	for (const auto& NodePair : Nodes)
	{
		const FGuid& NodeGuid = NodePair.Key;
		UFlowNode* Node = NodePair.Value;
		
		if (!Node)
		{
			continue;
		}
		
		OutText += FString::Printf(TEXT("Node: %s\n"), *NodeGuid.ToString());
		OutText += FString::Printf(TEXT("  Type: %s\n"), *Node->GetClass()->GetPathName());
		
		OutText += TEXT("  Properties:\n");
		
		UFlowNode* DefaultNode = Cast<UFlowNode>(Node->GetClass()->GetDefaultObject());
		
		for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			
			if (Property->GetName() == TEXT("NodeGuid") || 
				Property->GetName() == TEXT("InputPins") || 
				Property->GetName() == TEXT("OutputPins") ||
				Property->GetName() == TEXT("Connections"))
			{
				continue;
			}
			
			// Skip properties defined on base classes (UFlowNode or UFlowNodeBase)
			UClass* PropertyOwnerClass = Property->GetOwnerClass();
			if (PropertyOwnerClass == UFlowNode::StaticClass() || PropertyOwnerClass == UFlowNodeBase::StaticClass())
			{
				continue;
			}
			
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
			const void* DefaultValuePtr = DefaultNode ? Property->ContainerPtrToValuePtr<void>(DefaultNode) : nullptr;
			
			if (DefaultValuePtr && Property->Identical(ValuePtr, DefaultValuePtr))
			{
				continue;
			}
			
			FString ValueString = GetPropertyValueAsString(Property, ValuePtr);
			
			if (!ValueString.IsEmpty())
			{
				OutText += FString::Printf(TEXT("    %s: %s\n"), *Property->GetName(), *ValueString);
			}
		}
		
		const TArray<FFlowPin>& InputPins = Node->GetInputPins();
		OutText += TEXT("  InputPins:\n");
		if (InputPins.Num() == 0)
		{
			OutText += TEXT("    (none)\n");
		}
		else
		{
			for (const FFlowPin& Pin : InputPins)
			{
				OutText += FString::Printf(TEXT("    - %s [%s]\n"), 
					*Pin.PinName.ToString(), 
					*GetPinTypeName(Pin));
			}
		}
		
		const TArray<FFlowPin>& OutputPins = Node->GetOutputPins();
		OutText += TEXT("  OutputPins:\n");
		if (OutputPins.Num() == 0)
		{
			OutText += TEXT("    (none)\n");
		}
		else
		{
			for (const FFlowPin& Pin : OutputPins)
			{
				OutText += FString::Printf(TEXT("    - %s [%s]\n"), 
					*Pin.PinName.ToString(), 
					*GetPinTypeName(Pin));
			}
		}
		
		ExportNodeAddOns(Node, OutText);
		OutText += TEXT("\n");
	}
	
	OutText += TEXT("---\n\n");
}

void UFlowGraphExporter::ExportNodeAddOns(const UFlowNodeBase* Node, FString& OutText)
{
	const TArray<UFlowNodeAddOn*>& NodeAddOns = Node->GetFlowNodeAddOnChildren();
	if (NodeAddOns.IsEmpty())
	{
		return;
	}

	for (const UFlowNodeAddOn* AddOn : NodeAddOns)
	{
		if (!AddOn)
		{
			continue;
		}

		OutText += FString::Printf(TEXT("  AddOn: %s\n"), *AddOn->GetClass()->GetPathName());

		const UFlowNodeAddOn* DefaultAddOn = Cast<UFlowNodeAddOn>(AddOn->GetClass()->GetDefaultObject());

		for (TFieldIterator<FProperty> PropIt(AddOn->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;

			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			// Skip base-class infrastructure properties
			UClass* OwnerClass = Property->GetOwnerClass();
			if (OwnerClass == UFlowNodeBase::StaticClass() || OwnerClass == UFlowNodeAddOn::StaticClass())
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(AddOn);
			const void* DefaultValuePtr = DefaultAddOn ? Property->ContainerPtrToValuePtr<void>(DefaultAddOn) : nullptr;

			if (DefaultValuePtr && Property->Identical(ValuePtr, DefaultValuePtr))
			{
				continue;
			}

			FString ValueString = GetPropertyValueAsString(Property, ValuePtr);
			if (!ValueString.IsEmpty())
			{
				OutText += FString::Printf(TEXT("    %s: %s\n"), *Property->GetName(), *ValueString);
			}
		}
	}
}

void UFlowGraphExporter::ExportConnections(const UFlowAsset* FlowAsset, FString& OutText)
{
	OutText += TEXT("[CONNECTIONS]\n");
	
	const TMap<FGuid, UFlowNode*>& Nodes = FlowAsset->GetNodes();
	
	for (const auto& NodePair : Nodes)
	{
		const FGuid& NodeGuid = NodePair.Key;
		UFlowNode* Node = NodePair.Value;
		
		if (!Node)
		{
			continue;
		}
		
		// Export exec output pin connections (stored on source node with output pin name as key)
		const TArray<FFlowPin>& OutputPins = Node->GetOutputPins();
		for (const FFlowPin& OutputPin : OutputPins)
		{
			if (!OutputPin.IsExecPin())
			{
				continue;
			}
			
			FConnectedPin Connection = Node->GetConnection(OutputPin.PinName);
			if (Connection.NodeGuid.IsValid())
			{
				OutText += FString::Printf(TEXT("%s.%s -> %s.%s\n"),
					*NodeGuid.ToString(),
					*OutputPin.PinName.ToString(),
					*Connection.NodeGuid.ToString(),
					*Connection.PinName.ToString());
			}
		}
		
		// Export data input pin connections (stored on receiving node with input pin name as key)
		// These are stored as: InputPinName -> {SourceNodeGuid, SourceOutputPinName}
		// We need to reverse this to: SourceNodeGuid.SourceOutputPinName -> ThisNodeGuid.InputPinName
		const TArray<FFlowPin>& InputPins = Node->GetInputPins();
		for (const FFlowPin& InputPin : InputPins)
		{
			if (InputPin.IsExecPin())
			{
				continue;
			}
			
			FConnectedPin Connection = Node->GetConnection(InputPin.PinName);
			if (Connection.NodeGuid.IsValid())
			{
				// Reverse the connection direction for export
				// Connection stores: source node guid and source pin name
				// Export as: SourceNode.SourcePin -> ThisNode.InputPin
				OutText += FString::Printf(TEXT("%s.%s -> %s.%s\n"),
					*Connection.NodeGuid.ToString(),
					*Connection.PinName.ToString(),
					*NodeGuid.ToString(),
					*InputPin.PinName.ToString());
			}
		}
	}
	
	OutText += TEXT("---\n");
}

FString UFlowGraphExporter::GetPropertyValueAsString(const FProperty* Property, const void* ValuePtr)
{
	if (!Property || !ValuePtr)
	{
		return TEXT("");
	}
	
	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		return BoolProp->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
	}
	else if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		return FString::FromInt(IntProp->GetPropertyValue(ValuePtr));
	}
	else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		return FString::SanitizeFloat(FloatProp->GetPropertyValue(ValuePtr));
	}
	else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		return FString::Printf(TEXT("\"%s\""), *StrProp->GetPropertyValue(ValuePtr));
	}
	else if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		return NameProp->GetPropertyValue(ValuePtr).ToString();
	}
	else if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		return FString::Printf(TEXT("\"%s\""), *TextProp->GetPropertyValue(ValuePtr).ToString());
	}
	else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const UEnum* EnumType = EnumProp->GetEnum();
		if (EnumType)
		{
			int64 EnumValue = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return EnumType->GetNameStringByValue(EnumValue);
		}
	}
	else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (ByteProp->Enum)
		{
			uint8 ByteValue = ByteProp->GetPropertyValue(ValuePtr);
			return ByteProp->Enum->GetNameStringByValue(ByteValue);
		}
		else
		{
			return FString::FromInt(ByteProp->GetPropertyValue(ValuePtr));
		}
	}
	else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		UObject* Object = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (Object)
		{
			return Object->GetPathName();
		}
		return TEXT("None");
	}
	else if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		UClass* Class = Cast<UClass>(ClassProp->GetObjectPropertyValue(ValuePtr));
		if (Class)
		{
			return Class->GetPathName();
		}
		return TEXT("None");
	}
	else if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		FString StructString;
		StructProp->Struct->ExportText(StructString, ValuePtr, ValuePtr, nullptr, PPF_None, nullptr);
		return StructString;
	}
	else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		// Use Unreal's native ExportText for arrays to get proper format: (elem1,elem2,elem3)
		// This handles nested structs and TInstancedStruct correctly
		FString ArrayString;
		Property->ExportTextItem_Direct(ArrayString, ValuePtr, ValuePtr, nullptr, PPF_None);
		return ArrayString;
	}
	
	// For any other property types, use the generic ExportText
	FString ExportedText;
	Property->ExportTextItem_Direct(ExportedText, ValuePtr, ValuePtr, nullptr, PPF_None);
	return ExportedText;
}

FString UFlowGraphExporter::GetPinTypeName(const FFlowPin& Pin)
{
	const FFlowPinTypeName& PinTypeName = Pin.GetPinTypeName();
	return PinTypeName.ToString();
}
