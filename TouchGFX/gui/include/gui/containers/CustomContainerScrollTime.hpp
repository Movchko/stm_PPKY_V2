#ifndef CUSTOMCONTAINERSCROLLTIME_HPP
#define CUSTOMCONTAINERSCROLLTIME_HPP

#include <gui_generated/containers/CustomContainerScrollTimeBase.hpp>
#include <gui/containers/CustomContainerTime.hpp>

class CustomContainerScrollTime : public CustomContainerScrollTimeBase
{
public:
    CustomContainerScrollTime();
    virtual ~CustomContainerScrollTime() {}

    virtual void initialize();

    /**
     * Задать текущее время/дату. Контейнер сам переключает строку время/дата.
     */
    void setTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year);

    virtual void scrollWheel1UpdateItem(CustomContainerTime& item, int16_t itemIndex) override;

protected:
private:
    uint8_t timeHour;
    uint8_t timeMin;
    uint8_t timeSec;
    uint8_t timeDay;
    uint8_t timeMon;
    uint8_t timeYear;
};

#endif // CUSTOMCONTAINERSCROLLTIME_HPP
