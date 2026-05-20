#ifndef FRONTENDAPPLICATION_HPP
#define FRONTENDAPPLICATION_HPP

#include <gui_generated/common/FrontendApplicationBase.hpp>

class FrontendHeap;

using namespace touchgfx;

class FrontendApplication : public FrontendApplicationBase
{
public:
    FrontendApplication(Model& m, FrontendHeap& heap);
    virtual ~FrontendApplication() { }

    virtual void handleTickEvent() override;

    /** Переход на экран меню */
    void gotoScreenMenuScreenNoTransition();
    /** Переход на экран диспетчера устройств */
    void gotoScreenDevicesScreenNoTransition();

    /** Доступ к модели (для колбэков настроек и т.д.) */
    Model& getModel() { return model; }

private:
    void gotoScreenMenuScreenNoTransitionImpl();
    void gotoScreenDevicesScreenNoTransitionImpl();

    touchgfx::Callback<FrontendApplication> screenMenuTransitionCallback;
    touchgfx::Callback<FrontendApplication> screenDevicesTransitionCallback;

#ifndef SIMULATOR
    static const int NUM_BUTTONS = 7;
    uint8_t prevButtonStates[NUM_BUTTONS];
#endif
};

#endif // FRONTENDAPPLICATION_HPP
