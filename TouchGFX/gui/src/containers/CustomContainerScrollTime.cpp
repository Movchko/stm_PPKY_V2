#include <gui/containers/CustomContainerScrollTime.hpp>

CustomContainerScrollTime::CustomContainerScrollTime()
    : timeHour(0), timeMin(0), timeSec(0), timeDay(1), timeMon(1), timeYear(0)
{
}

void CustomContainerScrollTime::initialize()
{
    CustomContainerScrollTimeBase::initialize();
}

void CustomContainerScrollTime::setTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year)
{
    timeHour = hour;
    timeMin = min;
    timeSec = sec;
    timeDay = day;
    timeMon = month;
    timeYear = year;
    scrollWheel1.itemChanged(0);
    scrollWheel1.itemChanged(1);
}

void CustomContainerScrollTime::scrollWheel1UpdateItem(CustomContainerTime& item, int16_t itemIndex)
{
    item.updateText(static_cast<uint8_t>(itemIndex), timeSec, timeMin, timeHour, timeDay, timeMon, timeYear);
}
