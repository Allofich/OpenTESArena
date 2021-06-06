#ifndef CHARACTER_CREATION_UI_MODEL_H
#define CHARACTER_CREATION_UI_MODEL_H

#include <optional>
#include <string>

#include "../Math/Vector2.h"

class CharacterClassDefinition;
class Game;

namespace CharacterCreationUiModel
{
	std::string getPlayerName(Game &game);
	std::string getPlayerRaceName(Game &game);
	std::string getPlayerClassName(Game &game);

	std::string getChooseClassCreationTitleText(Game &game);
	std::string getGenerateClassButtonText(Game &game);
	std::string getGenerateClassButtonTooltipText();
	std::string getSelectClassButtonText(Game &game);
	std::string getSelectClassButtonTooltipText();

	std::string getChooseClassTitleText(Game &game);
	std::string getChooseClassArmorTooltipText(const CharacterClassDefinition &charClassDef);
	std::string getChooseClassShieldTooltipText(const CharacterClassDefinition &charClassDef);
	std::string getChooseClassWeaponTooltipText(const CharacterClassDefinition &charClassDef, Game &game);
	std::string getChooseClassFullTooltipText(const CharacterClassDefinition &charClassDef, Game &game);

	std::string getChooseGenderTitleText(Game &game);
	std::string getChooseGenderMaleText(Game &game);
	std::string getChooseGenderFemaleText(Game &game);

	std::string getChooseNameTitleText(Game &game);
	bool isPlayerNameCharacterAccepted(char c);

	std::string getChooseRaceTitleText(Game &game);
	std::string getChooseRaceProvinceConfirmTitleText(Game &game);
	std::string getChooseRaceProvinceTooltipText(Game &game, int provinceID);
	std::optional<int> getChooseRaceProvinceID(Game &game, const Int2 &originalPosition);

	std::string getChooseAttributesText(Game &game);

	std::string getAttributesMessageBoxTitleText(Game &game);
	std::string getAttributesMessageBoxSaveText(Game &game);
	std::string getAttributesMessageBoxRerollText(Game &game);

	std::string getAppearanceMessageBoxText(Game &game);
}

#endif