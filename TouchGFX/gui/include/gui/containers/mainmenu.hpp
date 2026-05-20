#ifndef MAINMENU_HPP
#define MAINMENU_HPP

#include <gui_generated/containers/mainmenuBase.hpp>

class mainmenu : public mainmenuBase
{
public:
    mainmenu();
    virtual ~mainmenu() {}
    void updateText(int16_t value);
    virtual void initialize();
protected:
};

#endif // MAINMENU_HPP
