#ifndef CUSTOMCONTAINERTIME_HPP
#define CUSTOMCONTAINERTIME_HPP

#include <gui_generated/containers/CustomContainerTimeBase.hpp>

class CustomContainerTime : public CustomContainerTimeBase
{
public:
    CustomContainerTime();
    virtual ~CustomContainerTime() {}
    void updateText(uint8_t val, uint8_t sec, uint8_t min, uint8_t hour, uint8_t day, uint8_t mon, uint8_t year);
    virtual void initialize();
protected:
};

#endif // CUSTOMCONTAINERTIME_HPP
