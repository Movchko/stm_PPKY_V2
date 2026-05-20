#ifndef SCREENMENUPRESENTER_HPP
#define SCREENMENUPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class ScreenMenuView;

class ScreenMenuPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    ScreenMenuPresenter(ScreenMenuView& v);

    /**
     * The activate function is called automatically when this screen is "switched in"
     * (ie. made active). Initialization logic can be placed here.
     */
    virtual void activate();

    /**
     * The deactivate function is called automatically when this screen is "switched out"
     * (ie. made inactive). Teardown functionality can be placed here.
     */
    virtual void deactivate();

    virtual ~ScreenMenuPresenter() {}
#ifndef SIMULATOR
    virtual void SetupMenuChangePos(unsigned char val);
    virtual void handleButton(uint8_t but, uint8_t state) override;
#endif
private:
    ScreenMenuPresenter();

    ScreenMenuView& view;

#ifndef SIMULATOR
    static const int MENU_ITEMS = 6;
    bool soundOn;  // состояние «ЗВУК» (пункт 1)
    int16_t currentIndex;
#endif
};

#endif // SCREENMENUPRESENTER_HPP
