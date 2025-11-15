#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <web-ifc/parsing/IfcLoader.h>
#include <web-ifc/schema/IfcSchemaManager.h>
#include <web-ifc/geometry/IfcGeometryProcessor.h>
#include <web-ifc/geometry/operations/geometryutils.h>
#include "classIDhasType.h"

#define FILE_SCHEMA 1109904537
using std::shared_ptr;

class BIMFile {
public:
	bool mIsLoaded = false;
	uint32_t mIfcProjectID = UINT32_MAX;                                          // Each IFC file has one entity with type IfcProject, this is its entityID
	std::string										mFilePath;                    // Path to the loaded IFC file
	shared_ptr<webifc::parsing::IfcLoader>			mLoader;                      // parser for ifc file, part of web-ifc. Contains the Ifc model after loading (property sets, relations, etc)
	shared_ptr<webifc::schema::IfcSchemaManager>	mSchemaManager;
	shared_ptr<webifc::geometry::IfcGeometryProcessor> mGeometryProcessor;        // processor ifc geometry, part of web-ifc
	bimGeometry::AABB mBBox;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t> > mMapElement2Children;    // [parentID] = { childID, relationID };
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t> > mMapElement2ParentRelationObject;  // [elementEntityID] = { parentID, relationID };
};

void traverseComposedMesh(shared_ptr<BIMFile>& bimFile, uint32_t elementID, uint32_t elementType,
	webifc::geometry::IfcComposedMesh& composedMesh, const glm::dmat4& parentMatrix, const glm::dvec4& parentColor, bool hasColor, std::string indent) {
	// Assuming VERTEX_FORMAT_SIZE_FLOATS == 6 for pos + normal
	constexpr size_t STRIDE = webifc::geometry::VERTEX_FORMAT_SIZE_FLOATS;

	if (!composedMesh.hasGeometry && composedMesh.children.size() == 0) {
		// empty node
		return;
	}

	glm::dvec4 newParentColor = parentColor;
	bool newHasColor = hasColor;
	if (composedMesh.hasColor) {
		newParentColor = composedMesh.color;
		newHasColor = true;
	}

	webifc::geometry::IfcGeometryProcessor& geometryProcessor = *bimFile->mGeometryProcessor;
	webifc::parsing::IfcLoader& loader = *bimFile->mLoader;
	if (!composedMesh.hasGeometry) {
		// could be #4867=IFCPRODUCTDEFINITIONSHAPE($,$,(#4858,#4866)); so jump over current node, attach children directly to parent node
		for (webifc::geometry::IfcComposedMesh& child : composedMesh.children) {
			uint32_t childEntityID = child.expressID;
			uint32_t childTypeCode = loader.GetLineType(childEntityID);
			childTypeCode = elementType;  // it it is a geometric item, don't use it as element type
			bool childHasColor = newHasColor;
			glm::dvec4 childColor = newParentColor;
			if (child.hasColor) {
				childColor = composedMesh.color;
				childHasColor = true;
			}
			traverseComposedMesh(bimFile, childEntityID, childTypeCode, child, parentMatrix, childColor, childHasColor, indent);
		}
		return;
	}

	glm::dmat4 newMatrix = parentMatrix * composedMesh.transformation;
	if (composedMesh.hasGeometry) {
		webifc::geometry::IfcGeometry geom = geometryProcessor.GetGeometry(composedMesh.expressID);

		std::cout << indent << "mesh ID: " << elementID << " has mesh with " << geom.numPoints << " points and " << geom.numFaces << " faces." << std::endl;

		bimGeometry::AABB meshBBox = geom.GetAABB();
		glm::dvec3 min = meshBBox.min;
		min = newMatrix * glm::dvec4(min, 1.0);
		glm::dvec3 max = meshBBox.max;
		max = newMatrix * glm::dvec4(max, 1.0);
		bimFile->mBBox.merge(min);
		bimFile->mBBox.merge(max);

		// Check for high coordinate values and offset if necessary
		for (uint32_t i = 0; i < geom.numPoints; ++i) {
			size_t pos_off = i * STRIDE;
			double x = geom.vertexData[pos_off + 0];
			double y = geom.vertexData[pos_off + 1];
			double z = geom.vertexData[pos_off + 2];

			glm::dvec3 pos(x, y, z);
			pos = newMatrix * glm::dvec4(pos, 1.0);
		}

		if (!composedMesh.hasColor) {
			newParentColor = parentColor;
		}
		else {
			newParentColor = composedMesh.color;
			newHasColor = composedMesh.hasColor;
		}
	}

	for (webifc::geometry::IfcComposedMesh& child : composedMesh.children) {
		uint32_t childEntityID = child.expressID;
		uint32_t childTypeCode = loader.GetLineType(childEntityID);
		if (!classIDhasType(childTypeCode, webifc::schema::IFCPRODUCT)) {
			childTypeCode = elementType;  // it it is a geometric item or IfcProductDefinitionShape, don't use it as element type
		}
		if (child.hasColor) {
			newParentColor = composedMesh.color;
			newHasColor = true;
		}
		std::string indentChild = indent + "  ";
		traverseComposedMesh(bimFile, childEntityID, childTypeCode, child, newMatrix, newParentColor, newHasColor, indentChild);
	}
}

void traverseElement(shared_ptr<BIMFile>& bimFile, uint32_t elementID, uint32_t typeCode, const glm::dmat4& parentMatrix, const glm::dvec4& parentColor, bool hasColor, std::string indent) {
	std::string elementTypeString = bimFile->mSchemaManager->IfcTypeCodeToType(typeCode); // just for debugging
	std::cout << indent << elementTypeString << std::endl;
	glm::dvec4 newParentColor = parentColor;
	bool newHasColor = hasColor;

	if (typeCode != webifc::schema::IFCPROJECT) {
		webifc::geometry::IfcComposedMesh composedMesh = bimFile->mGeometryProcessor->GetMesh(elementID);
		glm::dmat4 newMatrix = parentMatrix;

		if (composedMesh.hasGeometry || composedMesh.children.size() > 0) {
			if (composedMesh.hasColor) {
				newParentColor = composedMesh.color;
				newHasColor = true;
			}
			std::string indentChild = indent + "  ";
			traverseComposedMesh(bimFile, elementID, typeCode, composedMesh, newMatrix, newParentColor, newHasColor, indentChild);
		}
	}

	// Fetch the children from the IFC model
	auto it = bimFile->mMapElement2Children.find(elementID);
	if (it != bimFile->mMapElement2Children.end()) {
		std::unordered_map<uint32_t, uint32_t>& children = it->second;
		for (auto p : children) {
			uint32_t childEntityID = p.first;
			uint32_t childTypeCode = bimFile->mLoader->GetLineType(childEntityID);
			std::string childTypeStr = bimFile->mSchemaManager->IfcTypeCodeToType(childTypeCode);

			glm::dmat4 newMatrix = glm::dmat4(1.0);
			std::string indentChild = indent + "  ";
			traverseElement(bimFile, childEntityID, childTypeCode, newMatrix, newParentColor, newHasColor, indentChild);
		}
	}
}

//  Entry point  //////////////////////////////////////////////////////////////////
int main(){
	struct LoaderSettings {
		bool COORDINATE_TO_ORIGIN = false;
		uint16_t CIRCLE_SEGMENTS = 12;
		uint32_t TAPE_SIZE = 67108864; // 64 MByte, probably no need for anyone other than web-ifc devs to change this
		uint32_t MEMORY_LIMIT = UINT64_MAX * 0.5;
		uint16_t LINEWRITER_BUFFER = 10000;
	};

	LoaderSettings fileLoadingSettings;
	shared_ptr<BIMFile> bimFile = std::make_shared<BIMFile>();
	bimFile->mSchemaManager = std::make_shared<webifc::schema::IfcSchemaManager>();
	webifc::schema::IfcSchemaManager& schemaManager = *bimFile->mSchemaManager;
	bimFile->mLoader = std::make_shared<webifc::parsing::IfcLoader>(fileLoadingSettings.TAPE_SIZE, fileLoadingSettings.MEMORY_LIMIT, fileLoadingSettings.LINEWRITER_BUFFER, schemaManager);
	webifc::parsing::IfcLoader& loader = *bimFile->mLoader;
	
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

	// #18=IFCPROJECT('2Iicv0RnfAVPda6Sg4SE78',#5,'IfcOpenHouse',$,$,$,$,(#41,#47),#11);
	loader.MoveToArgumentOffset(ifcProjectID, 0); // GlobalId is argument 0
	auto tokenTypeGUID = loader.GetTokenType();
	loader.StepBack();
	if (tokenTypeGUID == webifc::parsing::STRING) {
		projectGUID = loader.GetStringArgument();   // read string "2Iicv0RnfAVPda6Sg4SE78"
	}

	loader.MoveToArgumentOffset(ifcProjectID, 2); // Name is argument 2
	auto tokenTypeName = loader.GetTokenType();
	loader.StepBack();
	if (tokenTypeName == webifc::parsing::STRING) {
		projectName = loader.GetStringArgument();  // read string "IfcOpenHouse"
	}

	// load project hierarchy: child-parent relations

	// Helper to extract parent and children from an IFC relation
	auto processRelation = [&](uint32_t relationID, uint32_t parentAttr, uint32_t childrenAttr) {
			// Read parent reference
			loader.MoveToArgumentOffset(relationID, parentAttr);
			auto tokenParent = loader.GetTokenType();
			loader.StepBack();
			if (tokenParent != webifc::parsing::REF) {
				// Parent missing -> skip
				return;
			}

			uint32_t parentTag = loader.GetRefArgument();  // RelatingObject in case of IfcRelAggregates, RelatingStructure in case of IfcRelContainedInSpatialStructure

			// Read children set
			loader.MoveToArgumentOffset(relationID, childrenAttr);
			auto tokenChildren = loader.GetTokenType();
			loader.StepBack();
			if (tokenChildren != webifc::parsing::SET_BEGIN) {
				// Could be SET_END for empty sets
				return;
			}

			const std::vector<uint32_t> tapeOffsets = loader.GetSetArgument();

			// insert into map for parent-child and child-parent relations
			for (uint32_t tapeOffset : tapeOffsets) {
				uint32_t childID = loader.GetRefArgument(tapeOffset);
				bimFile->mMapElement2ParentRelationObject[childID] = { parentTag, relationID };
				bimFile->mMapElement2Children[parentTag].emplace(childID, relationID);
			}
		};

	// IfcRelAggregates -----------------------------------------------------------
	//		IfcGloballyUniqueId					GlobalId;
	//		IfcOwnerHistory						OwnerHistory;				//optional
	//		IfcLabel							Name;						//optional
	//		IfcText								Description;				//optional
	//		IfcObjectDefinition					RelatingObject;				4
	//		std::vector<IfcObjectDefinition>	RelatedObjects;				5

	std::vector<uint32_t> relAggregates = loader.GetExpressIDsWithType(webifc::schema::IFCRELAGGREGATES);
	for (uint32_t id : relAggregates) {
		processRelation(id, 4, 5);
	}

	// IfcRelContainedInSpatialStructure ---------------------------------------------
	//		IfcGloballyUniqueId					GlobalId;
	//		IfcOwnerHistory						OwnerHistory;				//optional
	//		IfcLabel							Name;						//optional
	//		IfcText								Description;				//optional
	//		std::vector<IfcProduct>				RelatedElements;			4
	//		IfcSpatialElement					RelatingStructure;			5
	std::vector<uint32_t> relContained = loader.GetExpressIDsWithType(webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE);
	for (uint32_t id : relContained) {
		processRelation(id, 5, 4);
	}

	// traverse geometry
	bimFile->mGeometryProcessor = std::make_shared<webifc::geometry::IfcGeometryProcessor>(loader, schemaManager, fileLoadingSettings.CIRCLE_SEGMENTS, fileLoadingSettings.COORDINATE_TO_ORIGIN, 
		webifc::geometry::EPS_SMALL, webifc::geometry::EPS_SMALL, webifc::geometry::EPS_SMALL, webifc::geometry::EPS_TINY, webifc::geometry::EPS_SMALL, 4, 50);

	std::vector<uint32_t> project = loader.GetExpressIDsWithType(webifc::schema::IFCPROJECT);
	if (project.size() == 0) {
		std::cout << "No IfcProject entity found in the file.";
	}

	// remove the rotate z axis horizontal (NormalizeIFC) matrix which is default in web-ifc:
	webifc::geometry::NormalizeIFC = glm::dmat4(
		glm::dvec4(1, 0, 0, 0),
		glm::dvec4(0, 1, 0, 0),
		glm::dvec4(0, 0, 1, 0),
		glm::dvec4(0, 0, 0, 1));

	glm::dmat4 rootMatrix(1.0);
	glm::dvec4 baseColor(0.5, 0.5, 0.5, 1);
	std::string indent = "";
	traverseElement(bimFile, ifcProjectID, webifc::schema::IFCPROJECT, rootMatrix, baseColor, false, indent);
	glm::dvec3 min = bimFile->mBBox.min;
	glm::dvec3 max = bimFile->mBBox.max;
	std::cout << indent << "bbox min: (" << min.x << "/" << min.y << "/" << min.z << ")" << std::endl;
	std::cout << indent << "bbox max: (" << max.x << "/" << max.y << "/" << max.z << ")" << std::endl;

	return 0;
}
