#include <cmath>

#include "CharacterClass.h"
#include "CharacterClassCategoryName.h"
#include "../Items/ArmorMaterialType.h"
#include "../Items/ShieldType.h"

#include "components/debug/Debug.h"

CharacterClass::CharacterClass(const std::string &name,
	const std::string &preferredAttributes,
	const std::vector<ArmorMaterialType> &allowedArmors,
	const std::vector<ShieldType> &allowedShields,
	const std::vector<int> &allowedWeapons,
	CharacterClassCategoryName categoryName, double lockpicking, int healthDie,
	int initialExperienceCap, int classIndex, bool mage, bool thief, bool criticalHit)
	: name(name), preferredAttributes(preferredAttributes), allowedArmors(allowedArmors),
	allowedShields(allowedShields), allowedWeapons(allowedWeapons)
{
	this->categoryName = categoryName;
	this->lockpicking = lockpicking;
	this->healthDie = healthDie;
	this->initialExperienceCap = initialExperienceCap;
	this->classIndex = classIndex;
	this->mage = mage;
	this->thief = thief;
	this->criticalHit = criticalHit;
}

const std::string &CharacterClass::getName() const
{
	return this->name;
}

const std::string &CharacterClass::getPreferredAttributes() const
{
	return this->preferredAttributes;
}

const std::vector<ArmorMaterialType> &CharacterClass::getAllowedArmors() const
{
	return this->allowedArmors;
}

const std::vector<ShieldType> &CharacterClass::getAllowedShields() const
{
	return this->allowedShields;
}

const std::vector<int> &CharacterClass::getAllowedWeapons() const
{
	return this->allowedWeapons;
}

CharacterClassCategoryName CharacterClass::getCategoryName() const
{
	return this->categoryName;
}

double CharacterClass::getLockpicking() const
{
	return this->lockpicking;
}

int CharacterClass::getHealthDie() const
{
	return this->healthDie;
}

int CharacterClass::getInitialExperienceCap() const
{
	return this->initialExperienceCap;
}

int CharacterClass::getClassIndex() const
{
	return this->classIndex;
}

bool CharacterClass::canCastMagic() const
{
	return this->mage;
}

bool CharacterClass::isThief() const
{
	return this->thief;
}

bool CharacterClass::hasCriticalHit() const
{
	return this->criticalHit;
}

int CharacterClass::getExperienceCap(int level) const
{
	DebugAssert(level >= 0);

	if (level == 0)
	{
		return 0;
	}
	else if (level == 1)
	{
		return this->initialExperienceCap;
	}
	else
	{
		const int prevExperienceCap = this->getExperienceCap(level - 1);
		const double multiplier = ((level >= 2) && (level <= 8)) ? (30.0 / 16.0) : 1.50;
		return static_cast<int>(std::floor(
			static_cast<double>(prevExperienceCap) * multiplier));
	}
}
