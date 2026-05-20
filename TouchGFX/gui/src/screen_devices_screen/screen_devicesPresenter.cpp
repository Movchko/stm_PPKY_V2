#include <gui/screen_devices_screen/screen_devicesView.hpp>
#include <gui/screen_devices_screen/screen_devicesPresenter.hpp>
#include <gui/common/FrontendApplication.hpp>
#include <touchgfx/Application.hpp>
#ifndef SIMULATOR
#include "button.h"
#endif

screen_devicesPresenter::screen_devicesPresenter(screen_devicesView& v)
    : view(v)
{

}

void screen_devicesPresenter::activate()
{
#ifndef SIMULATOR
    view.refreshDeviceUi();
#endif
}

void screen_devicesPresenter::deactivate()
{

}

#ifndef SIMULATOR
void screen_devicesPresenter::handleButton(uint8_t but, uint8_t state)
{
    if (state != (uint8_t)ButtonStatePress) {
        return;
    }

    FrontendApplication* app = static_cast<FrontendApplication*>(touchgfx::Application::getInstance());
    if (but == BUT_ESC) {
        app->gotoScreenMenuScreenNoTransition();
        return;
    }
    if (but == BUT_UP) {
        view.prevDevice();
        return;
    }
    if (but == BUT_DOWN) {
        view.nextDevice();
        return;
    }
}
#endif
