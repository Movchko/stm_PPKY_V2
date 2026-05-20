#ifndef SCREENMENUVIEW_HPP
#define SCREENMENUVIEW_HPP

#include <gui_generated/screenmenu_screen/ScreenMenuViewBase.hpp>
#include <gui/screenmenu_screen/ScreenMenuPresenter.hpp>

class ScreenMenuView : public ScreenMenuViewBase
{
public:
    ScreenMenuView();
    virtual ~ScreenMenuView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /** Текущий выбранный пункт меню (0..5: РЕЖИМ, ЗВУК, ТЕСТ, СВЯЗЬ, ЖУРНАЛ, НАСТРОЙКИ) */
    int16_t getSelectedMenuIndex() const;

    /** Установить (прокрутить к) указанному пункту меню */
    void setMenuIndex(int16_t index);

    /** Обновить нижнюю строку (textAreatime_2): для пункта 1 (ЗВУК) — turnon/turnoff, для остальных — пусто/placeholder */
    void updateParameterLine(int16_t selectedIndex, bool soundOn);

#ifndef SIMULATOR
    virtual void SetupMenuChangePos(uint8_t val);
    virtual void scrollWheel1UpdateItem(mainmenu& item, int16_t itemIndex);
    virtual void scrollWheel1_1UpdateItem(mainmenu& item, int16_t itemIndex);
#endif
protected:
};

#endif // SCREENMENUVIEW_HPP
