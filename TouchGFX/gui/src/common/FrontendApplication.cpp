#include <gui/common/FrontendApplication.hpp>
#include <gui/common/FrontendHeap.hpp>
#include <gui/model/ModelListener.hpp>
#include <gui/mainscreen_screen/mainscreenView.hpp>
#include <gui/mainscreen_screen/mainscreenPresenter.hpp>
#include <gui/screenmenu_screen/ScreenMenuView.hpp>
#include <gui/screenmenu_screen/ScreenMenuPresenter.hpp>
#include <gui/screen_devices_screen/screen_devicesView.hpp>
#include <gui/screen_devices_screen/screen_devicesPresenter.hpp>
#include <touchgfx/transitions/NoTransition.hpp>

#ifndef SIMULATOR
#include "button.h"
#endif

using namespace touchgfx;

FrontendApplication::FrontendApplication(Model& m, FrontendHeap& heap)
    : FrontendApplicationBase(m, heap)
{
#ifndef SIMULATOR
    for (int i = 0; i < NUM_BUTTONS; i++)
        prevButtonStates[i] = 0;
#endif
}

void FrontendApplication::handleTickEvent()
{
    model.tick();

#ifndef SIMULATOR
    //Button_Process();
    ModelListener* listener = model.getModelListener();
    if (listener)
    {
        for (int but = 0; but < NUM_BUTTONS; but++)
        {
            uint8_t st = (uint8_t)Button_GetState((uint8_t)but);
            if (st == (uint8_t)ButtonStatePress && prevButtonStates[but] != (uint8_t)ButtonStatePress)
                listener->handleButton((uint8_t)but, st);
            prevButtonStates[but] = st;
        }
    }
#endif

    FrontendApplicationBase::handleTickEvent();
}

void FrontendApplication::gotoScreenMenuScreenNoTransition()
{
    screenMenuTransitionCallback = Callback<FrontendApplication>(this, &FrontendApplication::gotoScreenMenuScreenNoTransitionImpl);
    pendingScreenTransitionCallback = &screenMenuTransitionCallback;
}

void FrontendApplication::gotoScreenMenuScreenNoTransitionImpl()
{
    makeTransition<ScreenMenuView, ScreenMenuPresenter, NoTransition, Model>(&currentScreen, &currentPresenter, frontendHeap, &currentTransition, &model);
}

void FrontendApplication::gotoScreenDevicesScreenNoTransition()
{
    screenDevicesTransitionCallback = Callback<FrontendApplication>(this, &FrontendApplication::gotoScreenDevicesScreenNoTransitionImpl);
    pendingScreenTransitionCallback = &screenDevicesTransitionCallback;
}

void FrontendApplication::gotoScreenDevicesScreenNoTransitionImpl()
{
    makeTransition<screen_devicesView, screen_devicesPresenter, NoTransition, Model>(&currentScreen, &currentPresenter, frontendHeap, &currentTransition, &model);
}
