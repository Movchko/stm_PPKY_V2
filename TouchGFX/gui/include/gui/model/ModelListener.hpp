#ifndef MODELLISTENER_HPP
#define MODELLISTENER_HPP

#include <gui/model/Model.hpp>

class ModelListener
{
public:
    ModelListener() : model(0) {}
    
    virtual ~ModelListener() {}

    void bind(Model* m)
    {
        model = m;
    }
#ifndef SIMULATOR
    virtual void SetupMenuChangePos(unsigned char val) {};
    virtual void SetTime(long val) {};
    virtual void setDateTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year) {}
    /** Кнопка нажата: but = BUT_ENTER/BUT_ESC/BUT_UP/BUT_DOWN/..., state = ButtonStatePress */
    virtual void handleButton(uint8_t but, uint8_t state) {}

    /** Обновление состояния пожара: вызывается из модели по запросу приложения ППКУ. */
    virtual void onFireStatusChanged(bool active, uint8_t mode, uint8_t zone, uint8_t remaining_s, uint8_t nZoneNames,
				     char (*zoneNames)[ZONE_NAME_SIZE + 1]) {}
    virtual void onWarningStatusChanged(bool active, uint8_t nItems, char (*bigTitles)[WARNING_TITLE_LEN],
					char (*details)[ZONE_NAME_SIZE + 1]) {}
#endif
protected:
    Model* model;
};

#endif // MODELLISTENER_HPP
