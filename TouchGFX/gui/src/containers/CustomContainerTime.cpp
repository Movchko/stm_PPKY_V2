#include <gui/containers/CustomContainerTime.hpp>

CustomContainerTime::CustomContainerTime()
{

}

void CustomContainerTime::initialize()
{
    CustomContainerTimeBase::initialize();
}

#ifndef SIMULATOR
void CustomContainerTime::updateText(uint8_t val, uint8_t sec, uint8_t min, uint8_t hour, uint8_t day, uint8_t mon, uint8_t year) {
	switch(val) {
		case 0: {
			Unicode::snprintf(textAreatimeBuffer, TEXTAREATIME_SIZE, "%02i:%02i:%02i", hour, min, sec);
		}break;
		case 1: {
			Unicode::snprintf(textAreatimeBuffer, TEXTAREATIME_SIZE, "%02i.%02i.%02i", day, mon, year);
		}break;

	}


	textAreatime.invalidate();
}
#endif
