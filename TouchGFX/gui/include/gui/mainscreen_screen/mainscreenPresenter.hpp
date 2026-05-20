#ifndef MAINSCREENPRESENTER_HPP
#define MAINSCREENPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class mainscreenView;

class mainscreenPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    mainscreenPresenter(mainscreenView& v);

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

    virtual ~mainscreenPresenter() {}

    /** Передать время/дату на экран (из RTC/модели). */
    virtual void setDateTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year);

#ifndef SIMULATOR
    virtual void SetTime(uint32_t time);
    virtual void handleButton(uint8_t but, uint8_t state) override;

    /** Обновление состояния пожара для главного экрана. */
    virtual void onFireStatusChanged(bool active, uint8_t mode, uint8_t zone, uint8_t remaining_s, uint8_t nZoneNames,
				     char (*zoneNames)[ZONE_NAME_SIZE + 1]) override;
    virtual void onWarningStatusChanged(bool active, uint8_t nItems, char (*bigTitles)[WARNING_TITLE_LEN],
					char (*details)[ZONE_NAME_SIZE + 1]) override;
#endif
private:
    mainscreenPresenter();

    mainscreenView& view;
};

#endif // MAINSCREENPRESENTER_HPP
