// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphImporter.h"
#include "FlowLogChannels.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "AddOns/FlowNodeAddOn.h"
#include "Nodes/FlowNodeBase.h"
#include "Types/FlowNamedDataPinProperty.h"
#include "Types/FlowDataPinValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Graph/FlowGraph.h"

UFlowAsset* UFlowGraphImporter::ImportFlowGraphFromText(const FString& TargetAssetPath, const FString& ImportText, FString& OutErrorMessage)
{
	// TODO (gtaylor) Downgrade these logs to Verbose after the import/export mechanism has stabilized.
	UE_LOG(LogFlow, Log, TEXT("ImportFlowGraphFromText:\n%s"), *ImportText);

	OutErrorMessage.Empty();

	if (ImportText.IsEmpty())
	{
		OutErrorMessage = TEXT("Import text is empty");
		return nullptr;
	}

	// Parse header
	bool bWorldBound = true;
	FString ExpectedOwnerClass;
	FString UnusedAssetPath;
	FGuid UnusedAssetGuid;
	if (!ParseHeader(ImportText, UnusedAssetPath, UnusedAssetGuid, bWorldBound, ExpectedOwnerClass, OutErrorMessage))
	{
		return nullptr;
	}

	// Parse nodes
	TArray<FFlowGraphParsedNode> ParsedNodes;
	if (!ParseNodes(ImportText, ParsedNodes, OutErrorMessage))
	{
		return nullptr;
	}

	// Parse connections
	TArray<FFlowGraphParsedConnection> ParsedConnections;
	if (!ParseConnections(ImportText, ParsedConnections, OutErrorMessage))
	{
		return nullptr;
	}

	// Create the package and asset
	FString PackageName = FPackageName::ObjectPathToPackageName(TargetAssetPath);
	FString AssetName = FPackageName::ObjectPathToObjectName(TargetAssetPath);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}

	// Create the FlowAsset
	UFlowAsset* FlowAsset = NewObject<UFlowAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!FlowAsset)
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to create FlowAsset: %s"), *AssetName);
		return nullptr;
	}

	// Set basic properties
	FlowAsset->AssetGuid = FGuid::NewGuid();
	FlowAsset->bWorldBound = bWorldBound;

	// Create nodes
	TMap<FGuid, UFlowNode*> NodeMap;
	if (!CreateFlowNodes(FlowAsset, ParsedNodes, NodeMap, OutErrorMessage))
	{
		return nullptr;
	}

	// Setup connections
	if (!SetupConnections(FlowAsset, NodeMap, ParsedConnections, ParsedNodes, OutErrorMessage))
	{
		return nullptr;
	}

	// Create and attach AddOns
	if (!CreateNodeAddOns(NodeMap, ParsedNodes, OutErrorMessage))
	{
		return nullptr;
	}

	// Mark package dirty
	Package->MarkPackageDirty();

	return FlowAsset;
}

bool UFlowGraphImporter::PopulateFlowAssetFromText(UFlowAsset* TargetAsset, const FString& ImportText, FString& OutErrorMessage)
{
	UE_LOG(LogFlow, Log, TEXT("PopulateFlowAssetFromText: populating existing %s"), *TargetAsset->GetClass()->GetName());

	OutErrorMessage.Empty();

	if (!TargetAsset)
	{
		OutErrorMessage = TEXT("TargetAsset is null");
		return false;
	}

	if (ImportText.IsEmpty())
	{
		OutErrorMessage = TEXT("Import text is empty");
		return false;
	}

	// Parse header
	bool bWorldBound = true;
	FString ExpectedOwnerClass;
	FString UnusedAssetPath;
	FGuid UnusedAssetGuid;
	if (!ParseHeader(ImportText, UnusedAssetPath, UnusedAssetGuid, bWorldBound, ExpectedOwnerClass, OutErrorMessage))
	{
		return false;
	}

	// Parse nodes
	TArray<FFlowGraphParsedNode> ParsedNodes;
	if (!ParseNodes(ImportText, ParsedNodes, OutErrorMessage))
	{
		return false;
	}

	// Parse connections
	TArray<FFlowGraphParsedConnection> ParsedConnections;
	if (!ParseConnections(ImportText, ParsedConnections, OutErrorMessage))
	{
		return false;
	}

	// Clear existing editor graph nodes before rebuilding runtime nodes - this must happen first
	// so the graph does not hold stale references to the runtime nodes we are about to release.
	if (UFlowGraph* FlowGraph = Cast<UFlowGraph>(TargetAsset->GetGraph()))
	{
		FlowGraph->Nodes.Empty();
	}

	// Clear existing runtime nodes from the Nodes TMap via reflection (same approach used in CreateFlowNodes)
	FMapProperty* NodesProperty = FindFProperty<FMapProperty>(UFlowAsset::StaticClass(), TEXT("Nodes"));
	if (!NodesProperty)
	{
		OutErrorMessage = TEXT("Could not find Nodes property on UFlowAsset");
		return false;
	}

	void* NodesPtr = NodesProperty->ContainerPtrToValuePtr<void>(TargetAsset);
	FScriptMapHelper NodesMapHelper(NodesProperty, NodesPtr);
	NodesMapHelper.EmptyValues();

	// Apply header properties
	TargetAsset->bWorldBound = bWorldBound;

	// Create new runtime nodes
	TMap<FGuid, UFlowNode*> NodeMap;
	if (!CreateFlowNodes(TargetAsset, ParsedNodes, NodeMap, OutErrorMessage))
	{
		return false;
	}

	// Setup connections
	if (!SetupConnections(TargetAsset, NodeMap, ParsedConnections, ParsedNodes, OutErrorMessage))
	{
		return false;
	}

	// Create add-ons
	if (!CreateNodeAddOns(NodeMap, ParsedNodes, OutErrorMessage))
	{
		return false;
	}

	// Mark package dirty
	TargetAsset->GetPackage()->MarkPackageDirty();

	return true;
}

bool UFlowGraphImporter::ParseHeader(const FString& ImportText, FString& OutAssetPath, FGuid& OutAssetGuid, bool& OutWorldBound, FString& OutExpectedOwnerClass, FString& OutErrorMessage)
{
	TArray<FString> Lines;
	ImportText.ParseIntoArrayLines(Lines);

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i].TrimStartAndEnd();
		
		if (Line == TEXT("---"))
		{
			break;
		}

		if (Line.StartsWith(TEXT("WorldBound:")))
		{
			FString Value = Line.Mid(11).TrimStartAndEnd();
			OutWorldBound = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
		else if (Line.StartsWith(TEXT("ExpectedOwnerClass:")))
		{
			OutExpectedOwnerClass = Line.Mid(19).TrimStartAndEnd();
		}
	}

	return true;
}

bool UFlowGraphImporter::ParseNodes(const FString& ImportText, TArray<FFlowGraphParsedNode>& OutNodes, FString& OutErrorMessage)
{
	FString NodesSection = ExtractSection(ImportText, TEXT("[NODES]"));
	if (NodesSection.IsEmpty())
	{
		OutErrorMessage = TEXT("Could not find [NODES] section");
		return false;
	}

	TArray<FString> Lines;
	NodesSection.ParseIntoArrayLines(Lines);

	FFlowGraphParsedNode* CurrentNode = nullptr;
	FString CurrentSection;

	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		
		if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT(";")))
		{
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("Node:")))
		{
			// Start a new node
			CurrentNode = &OutNodes.AddDefaulted_GetRef();
			FString GuidString = TrimmedLine.Mid(5).TrimStartAndEnd();
			if (!ParseGuid(GuidString, CurrentNode->NodeGuid))
			{
				OutErrorMessage = FString::Printf(TEXT("Failed to parse node GUID: %s"), *GuidString);
				return false;
			}
			CurrentSection.Empty();
		}
		else if (TrimmedLine.StartsWith(TEXT("Type:")))
		{
			if (CurrentNode)
			{
				CurrentNode->NodeType = TrimmedLine.Mid(5).TrimStartAndEnd();
			}
		}
		else if (TrimmedLine == TEXT("Properties:"))
		{
			CurrentSection = TEXT("Properties");
		}
		else if (TrimmedLine == TEXT("InputPins:"))
		{
			CurrentSection = TEXT("InputPins");
		}
		else if (TrimmedLine == TEXT("OutputPins:"))
		{
			CurrentSection = TEXT("OutputPins");
		}
		else if (TrimmedLine == TEXT("AddOns:"))
		{
			CurrentSection = TEXT("AddOns");
		}
		else if (TrimmedLine.StartsWith(TEXT("AddOn:")) && CurrentNode)
		{
			// Start a new inline addon block: "AddOn: /Game/Path/To/FNA_Foo.FNA_Foo_C"
			FFlowGraphParsedNodeAddOn& NewAddOn = CurrentNode->AddOns.AddDefaulted_GetRef();
			NewAddOn.AddOnType = TrimmedLine.Mid(6).TrimStartAndEnd();
			CurrentSection = TEXT("AddOnProperties");
		}
		else if (CurrentNode && !CurrentSection.IsEmpty())
		{
			if (CurrentSection == TEXT("Properties"))
			{
				// Parse property line: "PropertyName: PropertyValue"
				int32 ColonIndex;
				if (TrimmedLine.FindChar(':', ColonIndex))
				{
					FString PropertyName = TrimmedLine.Left(ColonIndex).TrimStartAndEnd();
					FString PropertyValue = TrimmedLine.Mid(ColonIndex + 1).TrimStartAndEnd();
					CurrentNode->Properties.Add(PropertyName, PropertyValue);
				}
			}
			else if (CurrentSection == TEXT("InputPins") || CurrentSection == TEXT("OutputPins"))
			{
				// Parse pin line: "- PinName [PinType]"
				if (TrimmedLine.StartsWith(TEXT("-")))
				{
					FString PinInfo = TrimmedLine.Mid(1).TrimStartAndEnd();
					if (CurrentSection == TEXT("InputPins"))
					{
						CurrentNode->InputPins.Add(PinInfo);
					}
					else
					{
						CurrentNode->OutputPins.Add(PinInfo);
					}
				}
			}
			else if (CurrentSection == TEXT("AddOnProperties"))
			{
				// Parse addon property: "  Key: Value"
				// A new "AddOn:" or "Node:" line ends this section (handled above)
				int32 ColonIndex;
				if (CurrentNode->AddOns.Num() > 0 && TrimmedLine.FindChar(':', ColonIndex))
				{
					FString PropName = TrimmedLine.Left(ColonIndex).TrimStartAndEnd();
					FString PropValue = TrimmedLine.Mid(ColonIndex + 1).TrimStartAndEnd();
					CurrentNode->AddOns.Last().Properties.Add(PropName, PropValue);
				}
			}
		}
	}

	return true;
}

bool UFlowGraphImporter::ParseConnections(const FString& ImportText, TArray<FFlowGraphParsedConnection>& OutConnections, FString& OutErrorMessage)
{
	FString ConnectionsSection = ExtractSection(ImportText, TEXT("[CONNECTIONS]"));
	if (ConnectionsSection.IsEmpty())
	{
		return true;
	}

	TArray<FString> Lines;
	ConnectionsSection.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		
		if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT(";")))
		{
			continue;
		}

		FFlowGraphParsedConnection Connection;
		if (ParseConnectionLine(TrimmedLine, Connection, OutErrorMessage))
		{
			OutConnections.Add(Connection);
		}
		else if (!OutErrorMessage.IsEmpty())
		{
			return false;
		}
	}

	return true;
}

bool UFlowGraphImporter::CreateFlowNodes(UFlowAsset* FlowAsset, const TArray<FFlowGraphParsedNode>& ParsedNodes, TMap<FGuid, UFlowNode*>& OutNodeMap, FString& OutErrorMessage)
{
	// Use reflection to access the private Nodes map
	FMapProperty* NodesProperty = FindFProperty<FMapProperty>(UFlowAsset::StaticClass(), TEXT("Nodes"));
	if (!NodesProperty)
	{
		OutErrorMessage = TEXT("Could not find Nodes property on UFlowAsset");
		return false;
	}

	void* NodesPtr = NodesProperty->ContainerPtrToValuePtr<void>(FlowAsset);
	FScriptMapHelper NodesMapHelper(NodesProperty, NodesPtr);

	for (const FFlowGraphParsedNode& ParsedNode : ParsedNodes)
	{
		UClass* NodeClass = ResolveNodeClass(ParsedNode.NodeType);
		if (!NodeClass)
		{
			OutErrorMessage = FString::Printf(TEXT("Could not find node class: %s"), *ParsedNode.NodeType);
			return false;
		}

		// Create the node
		UFlowNode* NewNode = NewObject<UFlowNode>(FlowAsset, NodeClass, NAME_None, RF_Transactional);
		if (!NewNode)
		{
			OutErrorMessage = FString::Printf(TEXT("Failed to create node of type: %s"), *ParsedNode.NodeType);
			return false;
		}

		// Set the GUID
		NewNode->SetGuid(ParsedNode.NodeGuid);

		// Add to the FlowAsset's node map using reflection
		int32 Index = NodesMapHelper.AddDefaultValue_Invalid_NeedsRehash();
		uint8* PairPtr = NodesMapHelper.GetPairPtr(Index);
		
		// Set the key (GUID)
		FStructProperty* KeyProperty = CastField<FStructProperty>(NodesProperty->KeyProp);
		if (KeyProperty)
		{
			void* KeyPtr = KeyProperty->ContainerPtrToValuePtr<void>(PairPtr);
			KeyProperty->Struct->CopyScriptStruct(KeyPtr, &ParsedNode.NodeGuid);
		}
		
		// Set the value (UFlowNode*)
		FObjectProperty* ValueProperty = CastField<FObjectProperty>(NodesProperty->ValueProp);
		if (ValueProperty)
		{
			ValueProperty->SetObjectPropertyValue_InContainer(PairPtr, NewNode);
		}
		
		NodesMapHelper.Rehash();

		OutNodeMap.Add(ParsedNode.NodeGuid, NewNode);

		// Set properties
		if (!SetNodeProperties(NewNode, ParsedNode.Properties, OutErrorMessage))
		{
			return false;
		}
	}

	return true;
}

bool UFlowGraphImporter::SetNodeProperties(UFlowNode* FlowNode, const TMap<FString, FString>& Properties, FString& OutErrorMessage)
{
	for (const TPair<FString, FString>& PropertyPair : Properties)
	{
		// Try FindPropertyByName first (works for C++ properties)
		FProperty* Property = FlowNode->GetClass()->FindPropertyByName(*PropertyPair.Key);
		
		// If not found, try FindFProperty which works better with Blueprint properties
		if (!Property)
		{
			Property = FindFProperty<FProperty>(FlowNode->GetClass(), *PropertyPair.Key);
		}
		
		if (!Property)
		{
			UE_LOG(LogFlow, Warning, TEXT("Could not find property %s on node %s"), *PropertyPair.Key, *FlowNode->GetClass()->GetName());
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(FlowNode);
		if (!SetPropertyFromString(Property, ValuePtr, PropertyPair.Value, OutErrorMessage))
		{
			UE_LOG(LogFlow, Error, TEXT("Failed to set property %s on node %s: %s"), *PropertyPair.Key, *FlowNode->GetClass()->GetName(), *OutErrorMessage);
			OutErrorMessage.Empty();
		}
		else
		{
			UE_LOG(LogFlow, Log, TEXT("Successfully set property %s on node %s to: %s"), *PropertyPair.Key, *FlowNode->GetClass()->GetName(), *PropertyPair.Value);
		}
	}

	return true;
}

bool UFlowGraphImporter::SetupConnections(UFlowAsset* FlowAsset, const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedConnection>& Connections, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage)
{
	UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Setting up %d connections"), Connections.Num());

	const FFlowGraphDeclaredOutputPinTypes DeclaredOutputPinTypes = BuildDeclaredOutputPinTypes(ParsedNodes);

	FMapProperty* ConnectionsProperty = FindFProperty<FMapProperty>(UFlowNode::StaticClass(), TEXT("Connections"));
	if (!ConnectionsProperty)
	{
		OutErrorMessage = TEXT("Could not find Connections property on UFlowNode");
		UE_LOG(LogFlow, Error, TEXT("FlowGraphImporter: %s"), *OutErrorMessage);
		return false;
	}

	for (const FFlowGraphParsedConnection& Connection : Connections)
	{
		UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Processing connection %s.%s -> %s.%s"),
			*Connection.SourceNodeGuid.ToString(), *Connection.SourcePinName.ToString(),
			*Connection.TargetNodeGuid.ToString(), *Connection.TargetPinName.ToString());

		UFlowNode* SourceNode = NodeMap.FindRef(Connection.SourceNodeGuid);
		UFlowNode* TargetNode = NodeMap.FindRef(Connection.TargetNodeGuid);

		if (!SourceNode)
		{
			OutErrorMessage = FString::Printf(TEXT("Could not find source node: %s"), *Connection.SourceNodeGuid.ToString());
			UE_LOG(LogFlow, Error, TEXT("FlowGraphImporter: %s"), *OutErrorMessage);
			return false;
		}
		if (!TargetNode)
		{
			OutErrorMessage = FString::Printf(TEXT("Could not find target node: %s"), *Connection.TargetNodeGuid.ToString());
			UE_LOG(LogFlow, Error, TEXT("FlowGraphImporter: %s"), *OutErrorMessage);
			return false;
		}

		bool bIsExecPin = false;
		if (!ResolveSourcePin(SourceNode, Connection.SourcePinName, DeclaredOutputPinTypes, bIsExecPin))
		{
			UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: Could not resolve source pin %s on node %s — skipping connection"),
				*Connection.SourcePinName.ToString(), *SourceNode->GetClass()->GetName());
			continue;
		}

		if (!bIsExecPin)
		{
			EnsureTargetDataPinExists(TargetNode, Connection.TargetPinName);
		}

		// Exec connections key on the source output pin; data connections key on the target input pin.
		UFlowNode* NodeToModify    = bIsExecPin ? SourceNode : TargetNode;
		FName      KeyName         = bIsExecPin ? Connection.SourcePinName : Connection.TargetPinName;
		FConnectedPin ConnectedPin = bIsExecPin
			? FConnectedPin(Connection.TargetNodeGuid, Connection.TargetPinName)
			: FConnectedPin(Connection.SourceNodeGuid, Connection.SourcePinName);

		if (!WriteConnectionToNode(NodeToModify, KeyName, ConnectedPin, ConnectionsProperty))
		{
			UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: WriteConnectionToNode failed for key %s on %s"),
				*KeyName.ToString(), *NodeToModify->GetClass()->GetName());
		}
	}

	UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Finished setting up all connections"));
	return true;
}

FFlowGraphDeclaredOutputPinTypes UFlowGraphImporter::BuildDeclaredOutputPinTypes(const TArray<FFlowGraphParsedNode>& ParsedNodes)
{
	FFlowGraphDeclaredOutputPinTypes Result;
	for (const FFlowGraphParsedNode& ParsedNode : ParsedNodes)
	{
		TMap<FName, bool>& PinTypeMap = Result.FindOrAdd(ParsedNode.NodeGuid);
		for (const FString& PinDecl : ParsedNode.OutputPins)
		{
			FString PinName;
			FString PinType;
			if (PinDecl.Split(TEXT(" ["), &PinName, &PinType))
			{
				PinType.RemoveFromEnd(TEXT("]"));
				PinTypeMap.Add(FName(*PinName.TrimStartAndEnd()), PinType.TrimStartAndEnd().Equals(TEXT("Exec"), ESearchCase::IgnoreCase));
			}
		}
	}
	return Result;
}

bool UFlowGraphImporter::ResolveSourcePin(UFlowNode* SourceNode, FName SourcePinName, const FFlowGraphDeclaredOutputPinTypes& DeclaredOutputPinTypes, bool& bOutIsExecPin)
{
	// 1. Check live CDO pins first
	for (const FFlowPin& Pin : SourceNode->GetOutputPins())
	{
		if (Pin.PinName == SourcePinName)
		{
			bOutIsExecPin = Pin.IsExecPin();
			UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Found source pin %s on CDO, IsExec=%d"), *SourcePinName.ToString(), bOutIsExecPin);
			return true;
		}
	}

	// 2. Fall back to declared pins from the import text (handles dynamic pins absent from CDO)
	FArrayProperty* OutputPinsProperty = FindFProperty<FArrayProperty>(UFlowNode::StaticClass(), TEXT("OutputPins"));

	const TMap<FName, bool>* DeclaredPins = DeclaredOutputPinTypes.Find(SourceNode->GetGuid());
	if (DeclaredPins)
	{
		const bool* bDeclaredExec = DeclaredPins->Find(SourcePinName);
		if (bDeclaredExec)
		{
			bOutIsExecPin = *bDeclaredExec;
			UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Resolved dynamic source pin %s on %s as %s from declared pins"),
				*SourcePinName.ToString(), *SourceNode->GetClass()->GetName(), bOutIsExecPin ? TEXT("Exec") : TEXT("Data"));

			// Inject into the live OutputPins array so downstream code (CreateMissingDataPins etc.) can find it
			if (OutputPinsProperty)
			{
				FScriptArrayHelper Helper(OutputPinsProperty, OutputPinsProperty->ContainerPtrToValuePtr<void>(SourceNode));
				FFlowPin* NewPin = reinterpret_cast<FFlowPin*>(Helper.GetRawPtr(Helper.AddValue()));
				NewPin->PinName    = SourcePinName;
				NewPin->PinToolTip = SourcePinName.ToString();
				NewPin->SetPinTypeName(FFlowPinTypeName(bOutIsExecPin ? TEXT("Exec") : TEXT("wildcard")));
			}
			return true;
		}
	}

	// 3. Last resort: inject as wildcard data output
	UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: Source pin %s not found on %s — adding as wildcard data output"),
		*SourcePinName.ToString(), *SourceNode->GetClass()->GetName());

	if (OutputPinsProperty)
	{
		FScriptArrayHelper Helper(OutputPinsProperty, OutputPinsProperty->ContainerPtrToValuePtr<void>(SourceNode));
		FFlowPin* NewPin = reinterpret_cast<FFlowPin*>(Helper.GetRawPtr(Helper.AddValue()));
		NewPin->PinName    = SourcePinName;
		NewPin->PinToolTip = SourcePinName.ToString();
		NewPin->SetPinTypeName(FFlowPinTypeName(TEXT("wildcard")));
		bOutIsExecPin = false;
		return true;
	}

	UE_LOG(LogFlow, Error, TEXT("FlowGraphImporter: Could not find OutputPins property on UFlowNode"));
	return false;
}

bool UFlowGraphImporter::EnsureTargetDataPinExists(UFlowNode* TargetNode, FName TargetPinName)
{
	for (const FFlowPin& Pin : TargetNode->GetInputPins())
	{
		if (Pin.PinName == TargetPinName)
		{
			return true;
		}
	}

	UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: Target pin %s not found on %s — adding as wildcard data input"),
		*TargetPinName.ToString(), *TargetNode->GetClass()->GetName());

	FArrayProperty* InputPinsProperty = FindFProperty<FArrayProperty>(UFlowNode::StaticClass(), TEXT("InputPins"));
	if (!InputPinsProperty)
	{
		UE_LOG(LogFlow, Error, TEXT("FlowGraphImporter: Could not find InputPins property on UFlowNode"));
		return false;
	}

	FScriptArrayHelper Helper(InputPinsProperty, InputPinsProperty->ContainerPtrToValuePtr<void>(TargetNode));
	FFlowPin* NewPin = reinterpret_cast<FFlowPin*>(Helper.GetRawPtr(Helper.AddValue()));
	NewPin->PinName    = TargetPinName;
	NewPin->PinToolTip = TargetPinName.ToString();
	NewPin->SetPinTypeName(FFlowPinTypeName(TEXT("wildcard")));

	UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Added wildcard data input pin %s to %s"),
		*TargetPinName.ToString(), *TargetNode->GetClass()->GetName());
	return true;
}

bool UFlowGraphImporter::WriteConnectionToNode(UFlowNode* NodeToModify, FName KeyName, const FConnectedPin& ConnectedPin, FMapProperty* ConnectionsProperty)
{
	void* ConnectionsPtr = ConnectionsProperty->ContainerPtrToValuePtr<void>(NodeToModify);
	FScriptMapHelper MapHelper(ConnectionsProperty, ConnectionsPtr);

	int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
	uint8* PairPtr = MapHelper.GetPairPtr(Index);

	if (FNameProperty* KeyProp = CastField<FNameProperty>(ConnectionsProperty->KeyProp))
	{
		KeyProp->SetPropertyValue_InContainer(PairPtr, KeyName);
	}
	if (FStructProperty* ValueProp = CastField<FStructProperty>(ConnectionsProperty->ValueProp))
	{
		*static_cast<FConnectedPin*>(ValueProp->ContainerPtrToValuePtr<void>(PairPtr)) = ConnectedPin;
	}

	MapHelper.Rehash();

	UE_LOG(LogFlow, Log, TEXT("FlowGraphImporter: Wrote connection on %s key=%s -> %s.%s"),
		*NodeToModify->GetClass()->GetName(), *KeyName.ToString(),
		*ConnectedPin.NodeGuid.ToString(), *ConnectedPin.PinName.ToString());
	return true;
}

UClass* UFlowGraphImporter::ResolveNodeClass(const FString& NodeType)
{
	UClass* NodeClass = nullptr;

	// Full path (contains /Script/ or a dot) — try direct load
	if (NodeType.Contains(TEXT("/Script/")) || NodeType.Contains(TEXT(".")))
	{
		NodeClass = LoadObject<UClass>(nullptr, *NodeType);
	}

	// Short name — try /Script/Flow prefix
	if (!NodeClass)
	{
		NodeClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Flow.%s"), *NodeType));
	}
	if (!NodeClass)
	{
		NodeClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Flow.%s"), *NodeType));
	}

	// Last resort: as-is
	if (!NodeClass)
	{
		NodeClass = FindObject<UClass>(nullptr, *NodeType);
	}

	return NodeClass;
}

bool UFlowGraphImporter::CreateNodeAddOns(const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage)
{
	for (const FFlowGraphParsedNode& ParsedNode : ParsedNodes)
	{
		if (ParsedNode.AddOns.IsEmpty())
		{
			continue;
		}

		UFlowNode* OwnerNode = NodeMap.FindRef(ParsedNode.NodeGuid);
		if (!OwnerNode)
		{
			OutErrorMessage = FString::Printf(TEXT("CreateNodeAddOns: Could not find node for GUID %s"), *ParsedNode.NodeGuid.ToString());
			return false;
		}

		FArrayProperty* AddOnsProperty = FindFProperty<FArrayProperty>(UFlowNodeBase::StaticClass(), TEXT("AddOns"));
		if (!AddOnsProperty)
		{
			OutErrorMessage = TEXT("CreateNodeAddOns: Could not find AddOns property on UFlowNodeBase");
			return false;
		}

		for (const FFlowGraphParsedNodeAddOn& ParsedAddOn : ParsedNode.AddOns)
		{
			UClass* AddOnClass = nullptr;

			if (ParsedAddOn.AddOnType.Contains(TEXT("/Script/")) || ParsedAddOn.AddOnType.Contains(TEXT(".")))
			{
				AddOnClass = LoadObject<UClass>(nullptr, *ParsedAddOn.AddOnType);
			}

			if (!AddOnClass)
			{
				AddOnClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Flow.%s"), *ParsedAddOn.AddOnType));
			}

			if (!AddOnClass || !AddOnClass->IsChildOf(UFlowNodeAddOn::StaticClass()))
			{
				OutErrorMessage = FString::Printf(TEXT("CreateNodeAddOns: Could not find AddOn class: %s"), *ParsedAddOn.AddOnType);
				return false;
			}

			UFlowNodeAddOn* NewAddOn = NewObject<UFlowNodeAddOn>(OwnerNode, AddOnClass, NAME_None, RF_Transactional);
			if (!NewAddOn)
			{
				OutErrorMessage = FString::Printf(TEXT("CreateNodeAddOns: Failed to create AddOn of type: %s"), *ParsedAddOn.AddOnType);
				return false;
			}

			if (!SetAddOnProperties(NewAddOn, ParsedAddOn.Properties, OutErrorMessage))
			{
				return false;
			}

			// Append to the AddOns array via reflection (AddOns is protected)
			void* AddOnsPtr = AddOnsProperty->ContainerPtrToValuePtr<void>(OwnerNode);
			FScriptArrayHelper AddOnsHelper(AddOnsProperty, AddOnsPtr);
			int32 NewIndex = AddOnsHelper.AddValue();
			FObjectProperty* ElemProp = CastField<FObjectProperty>(AddOnsProperty->Inner);
			if (ElemProp)
			{
				ElemProp->SetObjectPropertyValue(AddOnsHelper.GetRawPtr(NewIndex), NewAddOn);
			}

			UE_LOG(LogFlow, Log, TEXT("CreateNodeAddOns: Attached %s to node %s"),
				*AddOnClass->GetName(), *OwnerNode->GetClass()->GetName());
		}
	}

	return true;
}

#if WITH_EDITORONLY_DATA
void UFlowGraphImporter::SyncAddOnDataPinValueDirections(UFlowNodeAddOn* AddOn)
{
	if (!AddOn)
	{
		return;
	}

	const UFlowNodeAddOn* CDO = Cast<UFlowNodeAddOn>(AddOn->GetClass()->GetDefaultObject());
	if (!CDO)
	{
		return;
	}

	static const UScriptStruct* NamedDataPinPropertyStruct = FFlowNamedDataPinProperty::StaticStruct();

	for (TFieldIterator<FStructProperty> PropIt(AddOn->GetClass()); PropIt; ++PropIt)
	{
		FStructProperty* StructProp = *PropIt;
		if (!StructProp || !StructProp->Struct->IsChildOf(NamedDataPinPropertyStruct))
		{
			continue;
		}

		FFlowNamedDataPinProperty* InstanceProp = StructProp->ContainerPtrToValuePtr<FFlowNamedDataPinProperty>(AddOn);
		const FFlowNamedDataPinProperty* CDOProp = StructProp->ContainerPtrToValuePtr<FFlowNamedDataPinProperty>(CDO);

		if (!InstanceProp || !CDOProp)
		{
			continue;
		}

		FFlowDataPinValue* InstanceValue = InstanceProp->DataPinValue.GetMutablePtr<FFlowDataPinValue>();
		const FFlowDataPinValue* CDOValue = CDOProp->DataPinValue.GetPtr<FFlowDataPinValue>();

		if (InstanceValue && CDOValue)
		{
			InstanceValue->bIsInputPin = CDOValue->bIsInputPin;
		}
	}
}
#endif

bool UFlowGraphImporter::SetAddOnProperties(UFlowNodeAddOn* AddOn, const TMap<FString, FString>& Properties, FString& OutErrorMessage)
{
	for (const TPair<FString, FString>& Pair : Properties)
	{
		FProperty* Property = AddOn->GetClass()->FindPropertyByName(*Pair.Key);
		if (!Property)
		{
			Property = FindFProperty<FProperty>(AddOn->GetClass(), *Pair.Key);
		}

		if (!Property)
		{
			UE_LOG(LogFlow, Warning, TEXT("SetAddOnProperties: Could not find property %s on %s"), *Pair.Key, *AddOn->GetClass()->GetName());
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(AddOn);
		if (!SetPropertyFromString(Property, ValuePtr, Pair.Value, OutErrorMessage))
		{
			UE_LOG(LogFlow, Warning, TEXT("SetAddOnProperties: Failed to set %s on %s: %s"), *Pair.Key, *AddOn->GetClass()->GetName(), *OutErrorMessage);
			OutErrorMessage.Empty();
		}
	}

#if WITH_EDITORONLY_DATA
	SyncAddOnDataPinValueDirections(AddOn);
#endif

	return true;
}

bool UFlowGraphImporter::SetPropertyFromString(FProperty* Property, void* ValuePtr, const FString& ValueString, FString& OutErrorMessage)
{
	if (!Property || !ValuePtr)
	{
		OutErrorMessage = TEXT("Invalid property or value pointer");
		return false;
	}

	// Handle different property types
	if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		bool bValue = ValueString.Equals(TEXT("true"), ESearchCase::IgnoreCase) || 
		              ValueString.Equals(TEXT("1"));
		BoolProperty->SetPropertyValue(ValuePtr, bValue);
		return true;
	}
	else if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
	{
		int32 Value = FCString::Atoi(*ValueString);
		IntProperty->SetPropertyValue(ValuePtr, Value);
		return true;
	}
	else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
	{
		float Value = FCString::Atof(*ValueString);
		FloatProperty->SetPropertyValue(ValuePtr, Value);
		return true;
	}
	else if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
	{
		StrProperty->SetPropertyValue(ValuePtr, ValueString);
		return true;
	}
	else if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		// Remove quotes if present
		FString CleanString = ValueString;
		if (CleanString.StartsWith(TEXT("\"")) && CleanString.EndsWith(TEXT("\"")))
		{
			CleanString = CleanString.Mid(1, CleanString.Len() - 2);
		}
		NameProperty->SetPropertyValue(ValuePtr, FName(*CleanString));
		return true;
	}
	else if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		// Handle NSLOCTEXT format: NSLOCTEXT("[namespace]", "key", "value")
		if (ValueString.StartsWith(TEXT("NSLOCTEXT")))
		{
			// Extract the display string (third parameter)
			int32 LastQuoteStart = ValueString.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			int32 SecondLastQuoteStart = ValueString.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd, LastQuoteStart - 1);
			
			if (LastQuoteStart != INDEX_NONE && SecondLastQuoteStart != INDEX_NONE)
			{
				FString DisplayString = ValueString.Mid(SecondLastQuoteStart + 1, LastQuoteStart - SecondLastQuoteStart - 1);
				TextProperty->SetPropertyValue(ValuePtr, FText::FromString(DisplayString));
				return true;
			}
		}
		else
		{
			// Simple string to text conversion
			FString CleanString = ValueString;
			if (CleanString.StartsWith(TEXT("\"")) && CleanString.EndsWith(TEXT("\"")))
			{
				CleanString = CleanString.Mid(1, CleanString.Len() - 2);
			}
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(CleanString));
			return true;
		}
	}
	else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		// Check if this is a TInstancedStruct - it needs special handling
		if (StructProperty->Struct->GetName().StartsWith(TEXT("InstancedStruct")))
		{
			// TInstancedStruct has its own ImportText that handles the (StructType,Fields) format
			const TCHAR* Buffer = *ValueString;
			if (Property->ImportText_Direct(Buffer, ValuePtr, nullptr, PPF_None))
			{
				return true;
			}
			else
			{
				OutErrorMessage = FString::Printf(TEXT("Failed to import TInstancedStruct property from text: %s"), *ValueString);
				UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: %s"), *OutErrorMessage);
				return false;
			}
		}
		else
		{
			// Regular struct - use the struct's ImportText
			if (StructProperty->Struct->ImportText(*ValueString, ValuePtr, nullptr, PPF_None, nullptr, StructProperty->Struct->GetName()))
			{
				return true;
			}
			else
			{
				OutErrorMessage = FString::Printf(TEXT("Failed to import struct property from text: %s"), *ValueString);
				return false;
			}
		}
	}
	else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		// Try to load the object
		UObject* Object = LoadObject<UObject>(nullptr, *ValueString);
		if (Object)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Object);
			return true;
		}
		else
		{
			OutErrorMessage = FString::Printf(TEXT("Failed to load object: %s"), *ValueString);
			return false;
		}
	}
	else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		const TCHAR* Buffer = *ValueString;
		if (ArrayProperty->ImportText_Direct(Buffer, ValuePtr, nullptr, PPF_None))
		{
			return true;
		}
		else
		{
			OutErrorMessage = FString::Printf(TEXT("Failed to import array property from text: %s"), *ValueString);
			UE_LOG(LogFlow, Warning, TEXT("FlowGraphImporter: %s"), *OutErrorMessage);
			return false;
		}
	}

	// For other types, try generic ImportText
	if (Property->ImportText_Direct(*ValueString, ValuePtr, nullptr, PPF_None))
	{
		return true;
	}

	OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s"), *Property->GetClass()->GetName());
	return false;
}

FString UFlowGraphImporter::ExtractSection(const FString& ImportText, const FString& SectionName)
{
	int32 SectionStart = ImportText.Find(SectionName);
	if (SectionStart == INDEX_NONE)
	{
		return FString();
	}

	// Find the end of the section (marked by "---")
	int32 SectionEnd = ImportText.Find(TEXT("---"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SectionStart + SectionName.Len());
	if (SectionEnd == INDEX_NONE)
	{
		SectionEnd = ImportText.Len();
	}

	return ImportText.Mid(SectionStart + SectionName.Len(), SectionEnd - SectionStart - SectionName.Len());
}

bool UFlowGraphImporter::ParseGuid(const FString& GuidString, FGuid& OutGuid)
{
	return FGuid::Parse(GuidString, OutGuid);
}

bool UFlowGraphImporter::ParseConnectionLine(const FString& Line, FFlowGraphParsedConnection& OutConnection, FString& OutErrorMessage)
{
	// Format: "GUID1.PinName1 -> GUID2.PinName2"
	int32 ArrowIndex = Line.Find(TEXT("->"));
	if (ArrowIndex == INDEX_NONE)
	{
		OutErrorMessage = FString::Printf(TEXT("Invalid connection line (missing '->'): %s"), *Line);
		return false;
	}

	FString SourcePart = Line.Left(ArrowIndex).TrimStartAndEnd();
	FString TargetPart = Line.Mid(ArrowIndex + 2).TrimStartAndEnd();

	// Parse source
	int32 SourceDotIndex;
	if (!SourcePart.FindChar('.', SourceDotIndex))
	{
		OutErrorMessage = FString::Printf(TEXT("Invalid connection source (missing '.'): %s"), *SourcePart);
		return false;
	}

	FString SourceGuidString = SourcePart.Left(SourceDotIndex);
	FString SourcePinString = SourcePart.Mid(SourceDotIndex + 1);

	if (!ParseGuid(SourceGuidString, OutConnection.SourceNodeGuid))
	{
		OutErrorMessage = FString::Printf(TEXT("Invalid source GUID: %s"), *SourceGuidString);
		return false;
	}

	OutConnection.SourcePinName = FName(*SourcePinString);

	// Parse target
	int32 TargetDotIndex;
	if (!TargetPart.FindChar('.', TargetDotIndex))
	{
		OutErrorMessage = FString::Printf(TEXT("Invalid connection target (missing '.'): %s"), *TargetPart);
		return false;
	}

	FString TargetGuidString = TargetPart.Left(TargetDotIndex);
	FString TargetPinString = TargetPart.Mid(TargetDotIndex + 1);

	if (!ParseGuid(TargetGuidString, OutConnection.TargetNodeGuid))
	{
		OutErrorMessage = FString::Printf(TEXT("Invalid target GUID: %s"), *TargetGuidString);
		return false;
	}

	OutConnection.TargetPinName = FName(*TargetPinString);

	return true;
}
