#ifndef CHOOSE_GENDER_PANEL_H
#define CHOOSE_GENDER_PANEL_H

#include <string>

#include "Button.h"
#include "Panel.h"
#include "../Entities/CharacterClass.h"
#include "../Rendering/Texture.h"

class Renderer;
class TextBox;

class ChooseGenderPanel : public Panel
{
private:
	Texture parchment;
	std::unique_ptr<TextBox> genderTextBox, maleTextBox, femaleTextBox;
	Button<Game&, const CharacterClass&> backToNameButton;
	Button<Game&, const CharacterClass&, const std::string&> maleButton, femaleButton;
	CharacterClass charClass;
	std::string name;
public:
	ChooseGenderPanel(Game &game, const CharacterClass &charClass,
		const std::string &name);
	virtual ~ChooseGenderPanel() = default;

	virtual Panel::CursorData getCurrentCursor() const override;
	virtual void handleEvent(const SDL_Event &e) override;
	virtual void render(Renderer &renderer) override;
};

#endif
