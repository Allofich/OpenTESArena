#ifndef DERIVED_ATTRIBUTE_H
#define DERIVED_ATTRIBUTE_H

#include <string>

enum class DerivedAttributeName;

// Derived attributes are a bit more complicated because, with health for example,
// it needs a base maximum that is calculated by some attribute, like endurance,
// and the gain in health per level is not retroactive. That functionality, however, 
// isn't implemented in this class because it does not have a polymorphic enough 
// design to satisfy all the different kinds of methods implied above. A manager 
// class will have the methods in it.
class DerivedAttribute
{
private:
	static const int MIN_VALUE;

	DerivedAttributeName attributeName;
	int current, maximum;
public:
	DerivedAttribute(DerivedAttributeName attributeName, int baseMaximum);

	int getCurrent() const;
	int getMaximum() const; // Might be a calculated value instead of a member in the future.
	DerivedAttributeName getAttributeName() const;
	std::string toString() const;

	// Perhaps there would be a "getCalculatedValue(EquippedItems..., StatusEffects...)", 
	// which would cap between the min and max value behind the scenes here.

	void setCurrent(int value);
	void setMaximum(int value);
};

#endif
