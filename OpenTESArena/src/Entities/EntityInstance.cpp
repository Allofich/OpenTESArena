#include "EntityInstance.h"

#include "components/debug/Debug.h"

EntityInstance::EntityInstance()
{
	this->clear();
}

void EntityInstance::init(EntityInstanceID instanceID, EntityPositionID positionID)
{
	DebugAssert(instanceID >= 0);
	DebugAssert(positionID >= 0);
	this->instanceID = instanceID;
	this->positionID = positionID;
}

void EntityInstance::clear()
{
	this->instanceID = -1;
	this->positionID = -1;
	this->directionID = -1;
	this->animDefID = -1;
	this->animInstID = -1;
}
