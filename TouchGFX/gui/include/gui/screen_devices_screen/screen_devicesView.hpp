#ifndef SCREEN_DEVICESVIEW_HPP
#define SCREEN_DEVICESVIEW_HPP

#include <gui_generated/screen_devices_screen/screen_devicesViewBase.hpp>
#include <gui/screen_devices_screen/screen_devicesPresenter.hpp>

class screen_devicesView : public screen_devicesViewBase
{
public:
    screen_devicesView();
    virtual ~screen_devicesView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

#ifndef SIMULATOR
    void refreshDeviceUi();
    void nextDevice();
    void prevDevice();
#endif
protected:
#ifndef SIMULATOR
    void renderSelected();
    uint8_t selectedIndex = 0u;
    uint8_t deviceSlots[32] = {0u};
    uint8_t deviceCount = 0u;
#endif
};

#endif // SCREEN_DEVICESVIEW_HPP
