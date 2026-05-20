#ifndef SCREEN_LOGOPRESENTER_HPP
#define SCREEN_LOGOPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class screen_logoView;

class screen_logoPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    screen_logoPresenter(screen_logoView& v);

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

    virtual ~screen_logoPresenter() {}

private:
    screen_logoPresenter();

    screen_logoView& view;
};

#endif // SCREEN_LOGOPRESENTER_HPP
