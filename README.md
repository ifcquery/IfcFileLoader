# IfcFileLoader

This is a sample project to demonstrate how to use web-ifc (https://github.com/ThatOpen/engine_web-ifc) in a native (non WASM) C++ project.

It demonstrates how to load an IFC file, traverse geometry &amp; meta data, with super high performance &amp; super low memory footprint

This repository contains project files for VS, to compile the application on Windows.

Compiling on Linux/Mac OS is also possible, but currently not part of the repository.

All external dependencies are in src/external, and already referenced in the VS project. No need to build those as separate libraries.


Check out a full IFC 3D viewer with the same technology under the hood: https://github.com/ifcquery/ifcsplitandmerge
<img width="600" alt="IfcSplitAndMerge: https://github.com/ifcquery/ifcsplitandmerge" src="https://github.com/user-attachments/assets/d6e976f9-c01c-4dd4-b15c-4aa9bda08810" />



### How to open an IFC file and read entity attributes with web-ifc

Web-ifc has a so called tape reader as internal data structure, i.e. not an object oriented model.
Web-ifc parses the STEP file content by loading the file content in chunks, then replacing the entity IDs with a uint32_t, the data type string with a CRC type code (uint32_t), and then the arguments according to their type.

<img width="1608" height="271" alt="image" src="https://github.com/user-attachments/assets/18c9afaa-8343-45c5-8fea-b6a0cdfc488e" />

So the text file content turns into a mix of binary 1-byte tokens, binary values like uint32_t for IDs, double values for floating point numbers, text remains text.

This approach results in a much smaller memory footprint compared to an object oriented model, and loading is much faster.

With the entity ID as offset, the loader jumps to any entity. With an offset [0, n] (n=number of arguments), the argument content can be read.



Here is a basic example how to open an IFC file and read directly from the tape (token stream):

```cpp
struct LoaderSettings {
	bool COORDINATE_TO_ORIGIN = false;
	uint16_t CIRCLE_SEGMENTS = 12;
	uint32_t TAPE_SIZE = 67108864; // 64 MByte, probably no need for anyone other than web-ifc devs to change this
	uint32_t MEMORY_LIMIT = UINT64_MAX * 0.5;
	uint16_t LINEWRITER_BUFFER = 10000;
};

webifc::schema::IfcSchemaManager schemaManager;
LoaderSettings fileLoadingSettings;
webifc::parsing::IfcLoader loader (fileLoadingSettings.TAPE_SIZE, fileLoadingSettings.MEMORY_LIMIT, fileLoadingSettings.LINEWRITER_BUFFER, schemaManager);


std::string fileName = "IfcOpenHouse_IFC4.ifc";
std::filesystem::path pathToFile = std::filesystem::absolute(fileName);
if (!std::filesystem::exists(pathToFile)) {
	std::cout << "File does not exist: " + pathToFile.string();
	return 0;
}

size_t fileSize = std::filesystem::file_size(pathToFile);


loader.LoadFile([&](char* dest, size_t sourceOffset, size_t destSize) {
	// this lambda is called for each chunk, so the content can be read from any source, stream etc.
	size_t length = std::min(static_cast<size_t>(fileSize - sourceOffset), destSize);
	std::ifstream file(pathToFile, std::ios::binary);
	if (file) {
		file.seekg(sourceOffset);
		file.read(dest, length);
		return static_cast<size_t>(file.gcount());
	}
	return size_t(0);
	});

std::vector<uint32_t> IfcProjectEntities = loader.GetExpressIDsWithType(webifc::schema::IFCPROJECT);
if (IfcProjectEntities.size() == 0) {
	std::cout << "No IfcProject entity found in the file.";
	return -1;
}
// IfcProject ----------------------------------------------------------------------------
//		IfcGloballyUniqueId							GlobalId;
//		IfcOwnerHistory								OwnerHistory;				//optional
//		IfcLabel									Name;						//optional
//		IfcText										Description;				//optional
//		IfcLabel									ObjectType;					//optional
//		IfcLabel									LongName;					//optional
//		IfcLabel									Phase;						//optional
//		std::vector<IfcRepresentationContext> 		RepresentationContexts;		//optional
//		IfcUnitAssignment							UnitsInContext;				//optional

uint32_t ifcProjectID = IfcProjectEntities[0];
std::string projectGUID = "";
std::string projectName = "DefaultProject";

// #12=IFCPROJECT('2Xw1_iVsn7OudAGdIJr3pp',#5,'IfcOpenHouse',$,$,$,$,(#41,#47),#11);
loader.MoveToArgumentOffset(ifcProjectID, 0); // GlobalId is argument 0
auto tokenTypeGUID = loader.GetTokenType();
loader.StepBack();
if (tokenTypeGUID == webifc::parsing::STRING) {
	projectGUID = loader.GetStringArgument();   // read string "2Xw1_iVsn7OudAGdIJr3pp"
}

loader.MoveToArgumentOffset(ifcProjectID, 2); // Name is argument 2
auto tokenTypeName = loader.GetTokenType();
loader.StepBack();
if (tokenTypeName == webifc::parsing::STRING) {
	projectName = loader.GetStringArgument();  // read string "IfcOpenHouse"
}

// load project hierarchy: child-parent relations
std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t> > mapElement2Children;    // [parentID] = { childID, relationID };
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t> > mapElement2ParentRelationObject;  // [elementEntityID] = { parentID, relationID };

std::vector<uint32_t> relAggregates = loader.GetExpressIDsWithType(webifc::schema::IFCRELAGGREGATES);
// IfcRelAggregates -----------------------------------------------------------
//		IfcGloballyUniqueId					GlobalId;
//		IfcOwnerHistory						OwnerHistory;				//optional
//		IfcLabel							Name;						//optional
//		IfcText								Description;				//optional
//		IfcObjectDefinition					RelatingObject;				4
//		std::vector<IfcObjectDefinition>	RelatedObjects;				5
for (auto relationID : relAggregates) {
	uint32_t parentAttributeNumber = 4;
	loader.MoveToArgumentOffset(relationID, parentAttributeNumber);
	auto tokenTypeParentEntity = loader.GetTokenType();
	loader.StepBack();
	if (tokenTypeParentEntity != webifc::parsing::REF) {
		// The argument is non-optional, but sometimes it is missing in IFC files anyway. Can be ignored safely
		continue;
	}
	uint32_t parentTag = loader.GetRefArgument();
	uint32_t setOfChildrenAttributeNumber = 5; // IfcRelContainedInSpatialStructure: 4
	loader.MoveToArgumentOffset(relationID, setOfChildrenAttributeNumber);
	auto tokenTypeChildren = loader.GetTokenType();
	loader.StepBack();
	if (tokenTypeChildren != webifc::parsing::SET_BEGIN) {
		// could be SET_END in case of an empty set
		continue;
	}
	const std::vector<uint32_t> RelatedElementsTapeOffsets = loader.GetSetArgument();
	for (const uint32_t& tapeOffset : RelatedElementsTapeOffsets) {
		uint32_t elementExpressID = loader.GetRefArgument(tapeOffset);
		uint32_t elementType = loader.GetLineType(elementExpressID);
		mapElement2ParentRelationObject[elementExpressID] = { parentTag, relationID };
		mapElement2Children[parentTag].emplace(elementExpressID, relationID);
	}
}
```


Download, open the sln file, compile & run:

<img width="2383" height="2054" alt="image" src="https://github.com/user-attachments/assets/c8d2cc78-e9af-448d-86be-5f9933ba79a2" />

Support and implementation services for web-ifc in desktop applications, as well as 3D graphics and Qt UI are available on www.IfcSplitAndMerge.com

There is also the existing web based application ecosystem and community for web-ifc: https://github.com/ThatOpen/engine_web-ifc


