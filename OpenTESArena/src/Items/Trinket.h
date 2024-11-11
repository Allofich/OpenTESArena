#ifndef TRINKET_H
#define TRINKET_H

#include <string>

#include "Item.h"

enum class TrinketType;

// Trinkets are non-metal accessories, so they don't inherit from Metallic.
class Trinket : public Item
{
private:
	TrinketType trinketType;
public:
	// There are no artifact trinkets for now, so this constructor remains simple.
	Trinket(TrinketType trinketType);
	~Trinket() = default;

	virtual ItemType getItemType() const override;
	virtual double getWeight() const override;
	virtual int getGoldValue() const override;
	virtual std::string getDisplayName() const override;

	TrinketType getTrinketType() const;
	int getMaxEquipCount() const;
};

#endif
