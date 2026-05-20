#ifndef MAINSCREENVIEW_HPP
#define MAINSCREENVIEW_HPP

#include <gui_generated/mainscreen_screen/mainscreenViewBase.hpp>
#include <gui/mainscreen_screen/mainscreenPresenter.hpp>

#ifndef SIMULATOR
#include "device_config.h"
#endif

class mainscreenView : public mainscreenViewBase
{
public:
    mainscreenView();
    virtual ~mainscreenView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
    virtual void handleTickEvent() override;

    /**
     * Передать текущее время/дату в контейнер часов.
     */
    void setDateTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year);

#ifndef SIMULATOR
    virtual void SetTime(uint32_t time);
#endif

#ifndef SIMULATOR
    /** Таймер + имена зон по очереди (одно имя, ротация 3 с после полного показа). */
    void updateFireStatus(bool active, uint8_t mode, uint8_t zone, uint8_t remaining_s, uint8_t nZoneNames,
			  char (*zoneNames)[ZONE_NAME_SIZE + 1]);
    void updateWarningStatus(bool active, uint8_t nItems, char (*bigTitles)[WARNING_TITLE_LEN],
			     char (*details)[ZONE_NAME_SIZE + 1]);

    /** Один полный проход бегущей строки (длинное имя) — пауза 3 с и смена зоны. */
    void fireOnMarqueeOnePassDone();

    /** Показать текущее имя зоны в бегущей строке (доступ к protected CustomContainerSrollText). */
    void fireShowCurrentZone();
    void warningOnMarqueeOnePassDone();
    void warningShowCurrent();
    void handleMainNavButton(uint8_t but);
    void uiSetWarningHeaderVisible(bool visible);
    void uiUpdateWarningHeader(uint8_t cur_idx, uint8_t total);
    void uiSetTopHeaderText(const char* text);
#endif

protected:
    bool fireUiActive = false;
};

#endif // MAINSCREENVIEW_HPP
