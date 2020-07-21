#include "SDL.h"

#include "ImagePanel.h"
#include "../Game/Game.h"
#include "../Media/PaletteFile.h"
#include "../Media/PaletteName.h"
#include "../Media/TextureFile.h"
#include "../Media/TextureManager.h"
#include "../Media/TextureName.h"
#include "../Rendering/Renderer.h"
#include "../Rendering/Texture.h"

ImagePanel::ImagePanel(Game &game, const std::string &paletteName, 
	const std::string &textureName, double secondsToDisplay,
	const std::function<void(Game&)> &endingAction)
	: Panel(game), paletteName(paletteName), textureName(textureName)
{
	this->skipButton = [&endingAction]()
	{
		return Button<Game&>(endingAction);
	}();

	this->secondsToDisplay = secondsToDisplay;
	this->currentSeconds = 0.0;
}

void ImagePanel::handleEvent(const SDL_Event &e)
{
	const auto &inputManager = this->getGame().getInputManager();
	bool leftClick = inputManager.mouseButtonPressed(e, SDL_BUTTON_LEFT);
	bool spacePressed = inputManager.keyPressed(e, SDLK_SPACE);
	bool enterPressed = inputManager.keyPressed(e, SDLK_RETURN) ||
		inputManager.keyPressed(e, SDLK_KP_ENTER);
	bool escapePressed = inputManager.keyPressed(e, SDLK_ESCAPE);

	bool skipHotkeyPressed = spacePressed || enterPressed || escapePressed;

	if (leftClick || skipHotkeyPressed)
	{
		this->skipButton.click(this->getGame());
	}	
}

void ImagePanel::tick(double dt)
{
	this->currentSeconds += dt;
	if (this->currentSeconds > this->secondsToDisplay)
	{
		this->skipButton.click(this->getGame());
	}
}

void ImagePanel::render(Renderer &renderer)
{
	// Clear full screen.
	renderer.clear();

	// Draw image.
	auto &textureManager = this->getGame().getTextureManager();
	const TextureID textureID = this->getTextureID(this->textureName, this->paletteName);
	const Texture &texture = textureManager.getTexture(textureID);
	renderer.drawOriginal(texture);
}
