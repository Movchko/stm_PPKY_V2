#include <gui/containers/mainmenu.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/Unicode.hpp>

mainmenu::mainmenu()
{

}

void mainmenu::initialize()
{
    mainmenuBase::initialize();
}
#ifndef SIMULATOR
void mainmenu::updateText(int16_t value)
{
	switch(value) {
		case 0: {
			Unicode::snprintf(textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE, "%s", touchgfx::TypedText(TEXTS(T_MODE)).getText());
		}break;
		case 1: {
			Unicode::snprintf(textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE, "%s", touchgfx::TypedText(TEXTS(T_SOUND)).getText());
		}break;
		case 2: {
			Unicode::fromUTF8(reinterpret_cast<const uint8_t*>("ДИСПЕТЧЕР"), textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE);
			textAreaMainMenuBuffer[TEXTAREAMAINMENU_SIZE - 1] = 0;
		}break;
		case 3: {
			Unicode::snprintf(textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE, "%s", touchgfx::TypedText(TEXTS(T_CONNECT)).getText());
		}break;
		case 4: {
			Unicode::snprintf(textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE, "%s", touchgfx::TypedText(TEXTS(T_JURNAL)).getText());
		}break;
		case 5: {
			Unicode::snprintf(textAreaMainMenuBuffer, TEXTAREAMAINMENU_SIZE, "%s", touchgfx::TypedText(TEXTS(T_SETTINGS)).getText());
		}break;

	}


	textAreaMainMenu.invalidate();
}
#endif
