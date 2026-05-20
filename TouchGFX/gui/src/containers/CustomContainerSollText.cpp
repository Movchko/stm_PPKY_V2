#include <gui/containers/CustomContainerSollText.hpp>
#include <touchgfx/Application.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <cstring>

using namespace touchgfx;

CustomContainerSollText::CustomContainerSollText()
    : marqueeX(0),
      marqueeTextWidth(0),
      marqueeRunning(false),
      marqueeFitsWidth(false),
      frameCountInteraction1Interval(0),
      delayframeCountInteraction1Interval(0),
      endDelayframeCountInteraction1Interval(0),
      marqueeEndX(0),
      finishedCallback(0)
{
}

void CustomContainerSollText::initialize()
{
    CustomContainerSollTextBase::initialize();

    // Спрятать статический текст, заданный через Designer
    textAreatime.setVisible(false);
    textAreatime.invalidate();

    // Настроить нашу бегущую строку во всю ширину контейнера
    marqueeText.setPosition(0, textAreatime.getY(),
                            getWidth(), textAreatime.getHeight());
    marqueeText.setColor(textAreatime.getColor());
    marqueeText.setLinespacing(0);
    // Используем TypedText с wildcard, как в CustomContainerTime
    marqueeText.setTypedText(TypedText(T___SINGLEUSE_D1VE));
    marqueeText.setWildcard(marqueeBuffer);
    marqueeText.setWideTextAction(WIDE_TEXT_NONE);

    // Пустая строка по умолчанию
    std::memset(marqueeBuffer, 0, sizeof(marqueeBuffer));

    add(marqueeText);

    // Регистрируем виджет для получения тиков
    Application::getInstance()->registerTimerWidget(this);
    frameCountInteraction1Interval = 0;
    delayframeCountInteraction1Interval = 0;
}

void CustomContainerSollText::setText(const char* text)
{
    if (!text)
    {
        std::memset(marqueeBuffer, 0, sizeof(marqueeBuffer));
        marqueeText.setWildcard(marqueeBuffer);
        marqueeText.invalidate();
        marqueeRunning = false;
        marqueeFitsWidth = false;
        marqueeTextWidth = 0;
        marqueeEndX = 0;
        delayframeCountInteraction1Interval = 0;
        endDelayframeCountInteraction1Interval = 0;
        invalidate();
        return;
    }

    const int16_t lineY = textAreatime.getY();
    const int16_t fallbackH = textAreatime.getHeight();

    std::memset(marqueeBuffer, 0, sizeof(marqueeBuffer));
    Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(text),
                      marqueeBuffer,
                      MARQUEE_BUFFER_SIZE);
    marqueeBuffer[MARQUEE_BUFFER_SIZE - 1] = 0;

    /* Явно обновить wildcard и габариты: иначе getTextWidth()/отрисовка остаются от прошлой строки */
    marqueeText.setWildcard(marqueeBuffer);
    marqueeText.resizeToCurrentText();

    uint16_t tw = marqueeText.getTextWidth();
    uint16_t th = marqueeText.getTextHeight();
    if (th == 0u) {
        th = (uint16_t)(fallbackH > 0 ? fallbackH : 1);
    }
    marqueeText.setPosition(0, lineY, (int16_t)tw, (int16_t)th);
    marqueeTextWidth = tw;

    const int16_t viewW = getWidth();
    marqueeFitsWidth = (marqueeTextWidth <= (uint16_t)viewW);
    marqueeEndX = (marqueeFitsWidth) ? 0 : (int16_t)(viewW - (int16_t)marqueeTextWidth);
    marqueeX = 0;
    marqueeText.moveTo(marqueeX, lineY);

    /* Длиннее области — бегущая строка; пустой или короткий текст — статично */
    marqueeRunning = (marqueeTextWidth > (uint16_t)viewW);
    frameCountInteraction1Interval = 0;
    delayframeCountInteraction1Interval = MARQUEE_START_DELAY_TICKS;
    endDelayframeCountInteraction1Interval = 0;

    marqueeText.invalidate();
    invalidate();
}

void CustomContainerSollText::restart()
{
    if (marqueeTextWidth <= 0)
    {
        return;
    }
    marqueeX = 0;
    marqueeText.moveTo(marqueeX, textAreatime.getY());
    marqueeRunning = true;
    frameCountInteraction1Interval = 0;
    delayframeCountInteraction1Interval = MARQUEE_START_DELAY_TICKS;
    endDelayframeCountInteraction1Interval = 0;
}

void CustomContainerSollText::handleTickEvent()
{
    if (!marqueeRunning || marqueeTextWidth <= 0)
    {
        return;
    }

    if(delayframeCountInteraction1Interval) {
    	delayframeCountInteraction1Interval--;
    	return;
    }

    if (endDelayframeCountInteraction1Interval > 0)
    {
        endDelayframeCountInteraction1Interval--;
        if (endDelayframeCountInteraction1Interval == 0)
        {
            if (finishedCallback)
            {
                marqueeRunning = false;
                finishedCallback(this);
            }
            else
            {
                marqueeX = 0;
                marqueeText.moveTo(marqueeX, textAreatime.getY());
                frameCountInteraction1Interval = 0;
                delayframeCountInteraction1Interval = MARQUEE_START_DELAY_TICKS;
            }
        }
        return;
    }


    frameCountInteraction1Interval++;
    if(frameCountInteraction1Interval >= MARQUEE_STEP_TICKS) {

		// Дошли до крайней полезной позиции: правый край текста показан полностью.
		if (marqueeX <= marqueeEndX)
		{
			marqueeX = marqueeEndX;
			marqueeText.moveTo(marqueeX, textAreatime.getY());
			endDelayframeCountInteraction1Interval = MARQUEE_START_DELAY_TICKS;
			frameCountInteraction1Interval = 0;
			return;
		}

		// Двигаем текст влево на 1 пиксель за тик
		marqueeX--;
		marqueeText.moveTo(marqueeX, textAreatime.getY());
		frameCountInteraction1Interval = 0;
    }
}
