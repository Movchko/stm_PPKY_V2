#include <gui/screenmenu_screen/ScreenMenuView.hpp>
#include <gui/screenmenu_screen/ScreenMenuPresenter.hpp>
#include <gui/common/FrontendApplication.hpp>
#include <touchgfx/Application.hpp>
#include "button.h"

ScreenMenuPresenter::ScreenMenuPresenter(ScreenMenuView& v)
    : view(v)
{
#ifndef SIMULATOR
    soundOn = true;  // фактическое значение возьмём из модели в activate()
    currentIndex = 0;
#endif
}

void ScreenMenuPresenter::activate()
{
#ifndef SIMULATOR
    FrontendApplication* app = static_cast<FrontendApplication*>(touchgfx::Application::getInstance());
    soundOn = app->getModel().getSoundOn();
    currentIndex = view.getSelectedMenuIndex();
    view.updateParameterLine(currentIndex, soundOn);
#endif
}

void ScreenMenuPresenter::deactivate()
{

}

#ifndef SIMULATOR
void ScreenMenuPresenter::SetupMenuChangePos(unsigned char val) {
	view.SetupMenuChangePos(val);
}

void ScreenMenuPresenter::handleButton(uint8_t but, uint8_t state)
{
    if (state != (uint8_t)ButtonStatePress)
        return;

    FrontendApplication* app = static_cast<FrontendApplication*>(touchgfx::Application::getInstance());

    if (but == BUT_ESC)
    {
        app->gotomainscreenScreenNoTransition();
        return;
    }

    if (but == BUT_UP)
    {
        currentIndex = (currentIndex - 1 + MENU_ITEMS) % MENU_ITEMS;
        view.setMenuIndex(currentIndex);
        view.updateParameterLine(currentIndex, soundOn);
        return;
    }

    if (but == BUT_DOWN)
    {
        currentIndex = (currentIndex + 1) % MENU_ITEMS;
        view.setMenuIndex(currentIndex);
        view.updateParameterLine(currentIndex, soundOn);
        return;
    }

    if (but == BUT_ENTER)
    {
        if (currentIndex == 1)  // ЗВУК
        {
            soundOn = !soundOn;
            view.updateParameterLine(currentIndex, soundOn);
            app->getModel().setSoundOn(soundOn);
            app->getModel().notifySoundToggled(soundOn);
            return;
        }
        if (currentIndex == 2)  // ДИСПЕТЧЕР УСТРОЙСТВ
        {
            app->gotoScreenDevicesScreenNoTransition();
            return;
        }
    }
}
#endif
