#ifndef ITEM_H
#define ITEM_H

#include <memory>
#include <string>

#include "ArtifactData.h"

enum class ItemType;

// I wanted to try and avoid using an abstract Item class, but in any case, this
// class should be used to try and bring together several elements, like the weight 
// or value of an item.
class Item
{
private:
	std::unique_ptr<ArtifactData> artifactData;
public:
	// Item constructor. Give null for artifact data if the item is not an artifact.
	Item(const ArtifactData *artifactData);
	virtual ~Item() = default;

	virtual std::unique_ptr<Item> clone() const = 0;

	const ArtifactData *getArtifactData() const;

	virtual ItemType getItemType() const = 0;
	virtual double getWeight() const = 0;
	virtual int getGoldValue() const = 0;
	virtual std::string getDisplayName() const = 0;

	// getTooltip()? How would it be split into colored sentences? Maybe a vector 
	// of TextLines. If unidentified, the text will be much reduced.

	// isQuestItem()?
};

#endif
