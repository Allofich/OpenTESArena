#include <algorithm>

#include "ImageSequencePanel.h"
#include "../Game/Game.h"
#include "../Input/InputActionMapName.h"
#include "../Input/InputActionName.h"
#include "../Media/TextureManager.h"
#include "../Rendering/ArenaRenderUtils.h"
#include "../Rendering/Renderer.h"

#include "components/debug/Debug.h"

ImageSequencePanel::ImageSequencePanel(Game &game)
	: Panel(game) { }

bool ImageSequencePanel::init(const std::vector<std::string> &paletteNames,
	const std::vector<std::string> &textureNames, const std::vector<double> &imageDurations,
	const OnFinishedFunction &onFinished)
{
	if (paletteNames.size() != textureNames.size())
	{
		DebugLogError("Palette names size (" + std::to_string(paletteNames.size()) +
			") doesn't match texture names size (" + std::to_string(textureNames.size()) + ").");
		return false;
	}

	if (paletteNames.size() != imageDurations.size())
	{
		DebugLogError("Palette names size (" + std::to_string(paletteNames.size()) +
			") doesn't match image durations size (" + std::to_string(imageDurations.size()) + ").");
		return false;
	}

	auto &game = this->getGame();

	this->skipButton = Button<Game&>(0, 0, ArenaRenderUtils::SCREEN_WIDTH, ArenaRenderUtils::SCREEN_HEIGHT,
		[this](Game &game)
	{
		this->currentSeconds = 0.0;

		const int imageCount = this->textureRefs.getCount();
		this->imageIndex = std::min(this->imageIndex + 1, imageCount);
		if (this->imageIndex == imageCount)
		{
			this->onFinished(game);
		}
	});

	this->addButtonProxy(MouseButtonType::Left, this->skipButton.getRect(),
		[this, &game]() { this->skipButton.click(game); });

	this->addInputActionListener(InputActionName::Skip,
		[this, &game](const InputActionCallbackValues &values)
	{
		if (values.performed)
		{
			this->onFinished(game);
		}
	});

	auto &textureManager = game.getTextureManager();
	auto &renderer = game.getRenderer();
	const int textureCount = static_cast<int>(textureNames.size());
	this->textureRefs.init(textureCount);
	for (int i = 0; i < textureCount; i++)
	{
		const std::string &textureName = textureNames[i]; // Assume single-image file.
		const std::string &paletteName = paletteNames[i];
		const TextureAsset textureAsset = TextureAsset(std::string(textureName));
		const TextureAsset paletteTextureAsset = TextureAsset(std::string(paletteName));

		UiTextureID textureID;
		if (!TextureUtils::tryAllocUiTexture(textureAsset, paletteTextureAsset, textureManager, renderer, &textureID))
		{
			DebugLogError("Couldn't create texture for image " + std::to_string(i) + " from \"" +
				textureName + "\" with palette \"" + paletteName + "\".");
			return false;
		}

		this->textureRefs.set(i, ScopedUiTextureRef(textureID, renderer));
	}

	UiDrawCall::TextureFunc textureFunc = [this]()
	{
		const ScopedUiTextureRef &textureRef = this->textureRefs.get(this->imageIndex);
		return textureRef.get();
	};

	this->addDrawCall(
		textureFunc,
		Int2::Zero,
		Int2(ArenaRenderUtils::SCREEN_WIDTH, ArenaRenderUtils::SCREEN_HEIGHT),
		PivotType::TopLeft);

	this->onFinished = onFinished;
	this->imageDurations = imageDurations;
	this->currentSeconds = 0.0;
	this->imageIndex = 0;
	return true;
}

void ImageSequencePanel::tick(double dt)
{
	const int imageCount = this->textureRefs.getCount();

	// Check if done iterating through images.
	if (this->imageIndex < imageCount)
	{
		this->currentSeconds += dt;

		// Step to the next image if its duration has passed.
		if (this->currentSeconds >= this->imageDurations.at(this->imageIndex))
		{
			this->currentSeconds = 0.0;
			this->imageIndex++;

			// Check if the last image is now over.
			if (this->imageIndex == imageCount)
			{
				this->onFinished(this->getGame());
			}
		}
	}

	// Clamp against the max so the index doesn't go outside the image vector.
	this->imageIndex = std::min(this->imageIndex, imageCount - 1);
}
