#ifndef SCREEN_LOGOVIEW_HPP
#define SCREEN_LOGOVIEW_HPP

#include <gui_generated/screen_logo_screen/screen_logoViewBase.hpp>
#include <gui/screen_logo_screen/screen_logoPresenter.hpp>

class screen_logoView : public screen_logoViewBase
{
public:
    screen_logoView();
    virtual ~screen_logoView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // SCREEN_LOGOVIEW_HPP
