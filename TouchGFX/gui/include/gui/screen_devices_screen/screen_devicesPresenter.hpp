#ifndef SCREEN_DEVICESPRESENTER_HPP
#define SCREEN_DEVICESPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class screen_devicesView;

class screen_devicesPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    screen_devicesPresenter(screen_devicesView& v);

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

    virtual ~screen_devicesPresenter() {}
#ifndef SIMULATOR
    virtual void handleButton(uint8_t but, uint8_t state) override;
#endif

private:
    screen_devicesPresenter();

    screen_devicesView& view;
};

#endif // SCREEN_DEVICESPRESENTER_HPP
