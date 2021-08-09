#ifndef LOGBOOK_PANEL_H
#define LOGBOOK_PANEL_H

#include "Panel.h"
#include "../UI/Button.h"
#include "../UI/TextBox.h"

class Renderer;

class LogbookPanel : public Panel
{
private:
	TextBox titleTextBox;
	Button<Game&> backButton;
public:
	LogbookPanel(Game &game);
	~LogbookPanel() override;

	bool init();

	virtual std::optional<CursorData> getCurrentCursor() const override;
	virtual void render(Renderer &renderer) override;
};

#endif
