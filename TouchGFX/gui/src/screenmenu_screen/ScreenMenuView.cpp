#include <gui/screenmenu_screen/ScreenMenuView.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/TypedText.hpp>
#include <cstdio>

ScreenMenuView::ScreenMenuView()
{

}

int16_t ScreenMenuView::getSelectedMenuIndex() const
{
    return (int16_t)scrollWheel1.getSelectedItem();
}

void ScreenMenuView::setMenuIndex(int16_t index)
{
    scrollWheel1.animateToItem(index, 10);
}

void ScreenMenuView::updateParameterLine(int16_t selectedIndex, bool soundOn)
{
    if (selectedIndex == 1) // ЗВУК
    {
        textAreatime_2.setTypedText(touchgfx::TypedText(soundOn ? T_TURNON : T_TURNOFF));
    }
    else
    {
        textAreatime_2.setTypedText(touchgfx::TypedText(T___SINGLEUSE_WSFU)); // placeholder/пусто
    }
    textAreatime_2.invalidate();
}

void ScreenMenuView::setupScreen()
{
    ScreenMenuViewBase::setupScreen();
#ifndef SIMULATOR
    for (int i = 0; i < scrollWheel1ListItems.getNumberOfDrawables(); i++)
    {
        scrollWheel1.itemChanged(i);
        scrollWheel1ListItems[i].updateText(i);
    }
    for (int i = 0; i < scrollWheel1_1ListItems.getNumberOfDrawables(); i++)
    {
        scrollWheel1_1.itemChanged(i);
        //scrollWheel1_1ListItems[i].updateText(i);
    }
#endif
}

void ScreenMenuView::tearDownScreen()
{
    ScreenMenuViewBase::tearDownScreen();
}
#ifndef SIMULATOR
void ScreenMenuView::SetupMenuChangePos(uint8_t val) {


	//if (val >= 0 && val < scrollWheel1.getNumberOfItems())
	//	scrollWheel1.animateToItem(val, -1);

	uint8_t i = 0;

	if(val >= 3)
		i = 1;


    scrollWheel1_1.itemChanged(i);
   // scrollWheel1_1ListItems[i].updateText(i, val+ 10, 39, 16, 3, 2, 26);


	//scrollWheel1_1.animateToItem(i, -1);


	//scrollWheel1.invalidate();
	//scrollWheel1.invalidateContent();
	//invalidate();


	//box1.invalidate();
	//scrollList1.animateToItem(val, 10);
	//scrollList1.invalidate();


/*
	if(val == 0)
		scrollableContainer2.reset();
	else {

		scrollableContainer2.doScroll(0, -val*64 );

	}

*/


	//int pos = scrollWheel1.getSelectedItem();




}

void ScreenMenuView::scrollWheel1UpdateItem(mainmenu& item, int16_t itemIndex)
{
    item.updateText(itemIndex);
}

void ScreenMenuView::scrollWheel1_1UpdateItem(mainmenu& item, int16_t itemIndex)
{
    item.updateText(itemIndex);
}

/*
void ScreenMenuView::scrollList1UpdateItem(mainmenu& item, int16_t itemIndex) {
	switch (itemIndex) {
		case 0: {

		}break;
	}
}
*/

#endif
//void ScreenMenuView::scrollWheel1UpdateItem(CustomContainer3& item, int16_t itemIndex) {

//}
