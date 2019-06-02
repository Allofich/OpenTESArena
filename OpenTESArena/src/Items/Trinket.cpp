#include <unordered_map>

#include "ItemType.h"
#include "Trinket.h"
#include "TrinketType.h"
#include "../Utilities/Debug.h"

const std::unordered_map<TrinketType, std::string> TrinketDisplayNames =
{
	{ TrinketType::Crystal, "Crystal" },
	{ TrinketType::Mark, "Mark" }
};

// These values are made up and should be revised sometime.
const std::unordered_map<TrinketType, double> TrinketWeights =
{
	{ TrinketType::Crystal, 0.25 },
	{ TrinketType::Mark, 0.20 }
};

// These values are made up and should be revised sometime.
const std::unordered_map<TrinketType, int> TrinketGoldValues =
{
	{ TrinketType::Crystal, 100 },
	{ TrinketType::Mark, 80 }
};

const std::unordered_map<TrinketType, int> TrinketMaxEquipCounts =
{
	{ TrinketType::Crystal, 1 },
	{ TrinketType::Mark, 1 }
};

Trinket::Trinket(TrinketType trinketType)
	: Item(nullptr)
{
	this->trinketType = trinketType;
}

ItemType Trinket::getItemType() const
{
	return ItemType::Trinket;
}

double Trinket::getWeight() const
{
	double weight = TrinketWeights.at(this->getTrinketType());
	DebugAssert(weight >= 0.0);
	return weight;
}

int Trinket::getGoldValue() const
{
	int baseValue = TrinketGoldValues.at(this->getTrinketType());
	return baseValue;
}

std::string Trinket::getDisplayName() const
{
	auto displayName = TrinketDisplayNames.at(this->getTrinketType());
	return displayName;
}

TrinketType Trinket::getTrinketType() const
{
	return this->trinketType;
}

int Trinket::getMaxEquipCount() const
{
	int maxCount = TrinketMaxEquipCounts.at(this->getTrinketType());
	return maxCount;
}
