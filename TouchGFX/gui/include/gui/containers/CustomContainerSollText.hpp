#ifndef CUSTOMCONTAINERSOLLTEXT_HPP
#define CUSTOMCONTAINERSOLLTEXT_HPP

#include <gui_generated/containers/CustomContainerSollTextBase.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
#include <touchgfx/Unicode.hpp>

class CustomContainerSollText : public CustomContainerSollTextBase
{
public:
    typedef void (*FinishedCallback)(CustomContainerSollText* sender);

    CustomContainerSollText();
    virtual ~CustomContainerSollText() {}

    virtual void initialize();

    /**
     * Задать текст бегущей строки (ASCII/UTF-8; буфер wildcard — см. MARQUEE_BUFFER_SIZE).
     */
    void setText(const char* text);

    /** Ширина текста после последнего setText (пиксели). */
    uint16_t getMarqueeTextWidth() const { return marqueeTextWidth; }

    /** true, если после setText текст не шире области — без прокрутки. */
    bool isMarqueeFitting() const { return marqueeFitsWidth; }

    /**
     * Установить колбэк, который вызывается один раз
     * после полной прокрутки текущего текста.
     * Если колбэк не задан — бегущая строка автоматически запускается заново.
     */
    void setFinishedCallback(FinishedCallback cb) { finishedCallback = cb; }

    /**
     * Принудительно запустить бегущую строку заново с начала (текущий текст).
     */
    void restart();

    virtual void handleTickEvent() override;

protected:
private:
    /* Пауза перед первым запуском прокрутки (тика TouchGFX). 200 тиков ~= 2с при 100 Гц. */
    static const int32_t MARQUEE_START_DELAY_TICKS = 200;
    /* Чем меньше значение, тем быстрее прокрутка. 2 = в 2 раза быстрее, чем было 4. */
    static const int32_t MARQUEE_STEP_TICKS = 2;
    static const uint16_t MARQUEE_BUFFER_SIZE = 129; // длинные имена зон (UTF-8) + терминатор

    touchgfx::TextAreaWithOneWildcard marqueeText;
    touchgfx::Unicode::UnicodeChar marqueeBuffer[MARQUEE_BUFFER_SIZE];

    int16_t marqueeX;
    uint16_t marqueeTextWidth;
    bool marqueeRunning;
    bool marqueeFitsWidth;
    int32_t frameCountInteraction1Interval;
    int32_t delayframeCountInteraction1Interval;
    int32_t endDelayframeCountInteraction1Interval;
    int16_t marqueeEndX;
    FinishedCallback finishedCallback;
};

#endif // CUSTOMCONTAINERSOLLTEXT_HPP
