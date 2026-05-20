#include <gui/mainscreen_screen/mainscreenView.hpp>
#include <cstdio>
#include <cstring>

#ifndef SIMULATOR
#include "main.h"
#include "device_config.h"
#include "button.h"
#include "fire.h"
#include "led.h"

namespace {

constexpr uint32_t FIRE_NAME_HOLD_MS = 3000u;
constexpr uint32_t MAIN_NAV_AUTO_RETURN_MS = (uint32_t)LED_BUT_IDLE_TIMEOUT_TICKS * 10u;

enum FireNamePhase : uint8_t {
	PH_IDLE = 0,
	PH_WAIT_LONG_SCROLL,
	PH_HOLD_3S
};

enum UiBannerMode : uint8_t {
	BANNER_NONE = 0,
	BANNER_FIRE,
	BANNER_WARNING
};

mainscreenView* g_fire_main_view = nullptr;

uint8_t s_fn_n = 0;
char s_fn_names[16][ZONE_NAME_SIZE + 1];
uint8_t s_fn_cur = 0;
FireNamePhase s_fn_ph = PH_IDLE;
uint32_t s_fn_hold_from = 0;
UiBannerMode s_banner_mode = BANNER_NONE;

uint8_t s_wn_n = 0;
char s_wn_titles[16][WARNING_TITLE_LEN];
char s_wn_details[16][ZONE_NAME_SIZE + 1];
uint8_t s_wn_is_attention[16] = {0u};
uint8_t s_wn_cur = 0;
FireNamePhase s_wn_ph = PH_IDLE;
uint32_t s_wn_hold_from = 0;
char s_top_header_text[24] = {0};
uint8_t s_fire_manual_select = 0u;
uint8_t s_warning_manual_select = 0u;
uint8_t s_fire_mode = 0u;
uint32_t s_fire_nav_last_press_ms = 0u;
uint32_t s_warning_nav_last_press_ms = 0u;

static void fire_copy_list(uint8_t n, char (*src)[ZONE_NAME_SIZE + 1])
{
	s_fn_n = (n > 16u) ? 16u : n;
	for (uint8_t i = 0u; i < s_fn_n; i++) {
		std::strncpy(s_fn_names[i], src[i], ZONE_NAME_SIZE);
		s_fn_names[i][ZONE_NAME_SIZE] = '\0';
	}
}

static bool fire_list_equals(uint8_t n, char (*src)[ZONE_NAME_SIZE + 1])
{
	if (n != s_fn_n) {
		return false;
	}
	for (uint8_t i = 0u; i < n; i++) {
		if (std::strncmp(s_fn_names[i], src[i], ZONE_NAME_SIZE + 1) != 0) {
			return false;
		}
	}
	return true;
}

static void fire_marquee_done_thunk(CustomContainerSollText*)
{
	if (g_fire_main_view != nullptr) {
		if (s_banner_mode == BANNER_FIRE) {
			g_fire_main_view->fireOnMarqueeOnePassDone();
		} else if (s_banner_mode == BANNER_WARNING) {
			g_fire_main_view->warningOnMarqueeOnePassDone();
		}
	}
}

static void ui_set_warning_header_visible(mainscreenView* view, bool visible)
{
	if (view == nullptr) {
		return;
	}
	view->uiSetWarningHeaderVisible(visible);
}

static void ui_update_warning_header(mainscreenView* view)
{
	if (view == nullptr || s_wn_n == 0u) {
		return;
	}
	view->uiUpdateWarningHeader((uint8_t)(s_wn_cur + 1u), s_wn_n);
}

} // namespace
#endif

mainscreenView::mainscreenView()
{

}

void mainscreenView::setupScreen()
{
    mainscreenViewBase::setupScreen();

    /* По умолчанию пустая бегущая строка, будет задаваться из приложения (пожар, ошибки и т.п.) */
    CustomContainerSrollText.setText("");
#ifndef SIMULATOR
    g_fire_main_view = this;
    CustomContainerSrollText.setFinishedCallback(fire_marquee_done_thunk);
    /* Экран пересоздаётся после выхода из меню: сбрасываем runtime-фазы,
     * чтобы рендер баннеров (пожар/авария) гарантированно стартовал заново
     * даже при неизменном списке элементов. */
    s_banner_mode = BANNER_NONE;
    s_fn_ph = PH_IDLE;
    s_fn_hold_from = 0u;
    s_wn_ph = PH_IDLE;
    s_wn_hold_from = 0u;
    ui_set_warning_header_visible(this, false);
    s_fire_manual_select = 0u;
    s_warning_manual_select = 0u;
    s_fire_nav_last_press_ms = 0u;
    s_warning_nav_last_press_ms = 0u;
    Fire_UiSetManualSelection(0u, 0u);
#endif
}

void mainscreenView::tearDownScreen()
{
#ifndef SIMULATOR
    if (g_fire_main_view == this) {
        g_fire_main_view = nullptr;
    }
    CustomContainerSrollText.setFinishedCallback(nullptr);
#endif
    mainscreenViewBase::tearDownScreen();
}

void mainscreenView::setDateTime(uint8_t hour, uint8_t min, uint8_t sec, uint8_t day, uint8_t month, uint8_t year)
{
#ifndef SIMULATOR
    if (textAreatime_top_bar.isVisible()) {
        return;
    }
#endif
    customContainerScrollTime1.setTime(hour, min, sec, day, month, year);
}

#ifndef SIMULATOR
void mainscreenView::fireOnMarqueeOnePassDone()
{
	if (s_fn_ph == PH_WAIT_LONG_SCROLL) {
		s_fn_ph = PH_HOLD_3S;
		s_fn_hold_from = HAL_GetTick();
	}
}

void mainscreenView::fireShowCurrentZone()
{
	if (s_fn_n == 0u) {
		return;
	}
	CustomContainerSrollText.setText(s_fn_names[s_fn_cur]);
	s_banner_mode = BANNER_FIRE;
	if (s_fire_manual_select) {
		/* В ручном выборе пожара верхняя строка должна быть как в штатном режиме пожара. */
		if (s_fire_mode == 1u || s_fire_mode == 5u) {
			ui_set_warning_header_visible(this, true);
			uiSetTopHeaderText((s_fire_mode == 5u) ? "ПАУЗА" : "ДО ПУСКА");
		} else if (s_fire_mode == 6u) {
			ui_set_warning_header_visible(this, true);
			uiSetTopHeaderText("");
		} else {
			ui_set_warning_header_visible(this, false);
		}
		s_fn_ph = PH_IDLE;
		s_fn_hold_from = 0u;
		return;
	}
	if (CustomContainerSrollText.isMarqueeFitting()) {
		s_fn_ph = PH_HOLD_3S;
		s_fn_hold_from = HAL_GetTick();
	} else {
		s_fn_ph = PH_WAIT_LONG_SCROLL;
	}
}

void mainscreenView::warningOnMarqueeOnePassDone()
{
	if (s_wn_ph != PH_WAIT_LONG_SCROLL || s_wn_n == 0u) {
		return;
	}
	/* Для длинных строк переключаемся сразу на следующую неисправность:
	 * пауза перед новым стартом обеспечивается самим контейнером (2с). */
	s_wn_cur = (uint8_t)((s_wn_cur + 1u) % s_wn_n);
	s_wn_ph = PH_IDLE;
	s_wn_hold_from = 0u;
	ui_update_warning_header(this);
}

void mainscreenView::warningShowCurrent()
{
	if (s_wn_n == 0u) {
		return;
	}
	s_banner_mode = BANNER_WARNING;
	ui_set_warning_header_visible(this, true);
	ui_update_warning_header(this);

	for (uint16_t i = 0; i < TEXTAREA1_SIZE; i++) {
		textArea1Buffer[i] = 0;
	}
	Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(s_wn_titles[s_wn_cur]), textArea1Buffer, TEXTAREA1_SIZE);
	textArea1Buffer[TEXTAREA1_SIZE - 1] = 0;
	textArea1.setWildcard(textArea1Buffer);
	textArea1.invalidate();

	CustomContainerSrollText.setText(s_wn_details[s_wn_cur]);
	if (s_warning_manual_select) {
		s_wn_ph = PH_IDLE;
		s_wn_hold_from = 0u;
		return;
	}
	if (CustomContainerSrollText.isMarqueeFitting()) {
		s_wn_ph = PH_HOLD_3S;
		s_wn_hold_from = HAL_GetTick();
	} else {
		s_wn_ph = PH_WAIT_LONG_SCROLL;
	}
}

void mainscreenView::SetTime(uint32_t time) {

};

void mainscreenView::handleTickEvent()
{
	mainscreenViewBase::handleTickEvent();
#ifndef SIMULATOR
	uint32_t now = HAL_GetTick();
	if (s_fire_manual_select &&
	    s_fire_nav_last_press_ms != 0u &&
	    (now - s_fire_nav_last_press_ms) >= MAIN_NAV_AUTO_RETURN_MS) {
		s_fire_manual_select = 0u;
		Fire_UiSetManualSelection(0u, 0u);
		s_fn_ph = PH_IDLE;
		s_fn_hold_from = 0u;
		s_fire_nav_last_press_ms = 0u;
		fireShowCurrentZone();
	}
	if (s_warning_manual_select &&
	    s_warning_nav_last_press_ms != 0u &&
	    (now - s_warning_nav_last_press_ms) >= MAIN_NAV_AUTO_RETURN_MS) {
		s_warning_manual_select = 0u;
		s_wn_ph = PH_IDLE;
		s_wn_hold_from = 0u;
		s_warning_nav_last_press_ms = 0u;
		warningShowCurrent();
	}
#endif
}

void mainscreenView::uiSetWarningHeaderVisible(bool visible)
{
	bool cur = textAreatime_top_bar.isVisible();
	if (cur == visible) {
		return;
	}
	customContainerTopBar1.setVisible(!visible);
	customContainerTopBar1.invalidate();
	customContainerScrollTime1.setVisible(!visible);
	customContainerScrollTime1.invalidate();
	textAreatime_top_bar.setVisible(visible);
	textAreatime_top_bar.invalidate();
}

void mainscreenView::uiUpdateWarningHeader(uint8_t cur_idx, uint8_t total)
{
	char hdr[24];
	if (s_wn_n > 0u && s_wn_cur < s_wn_n && s_wn_is_attention[s_wn_cur]) {
		snprintf(hdr, sizeof(hdr), "ВНИМАНИЕ");
	} else {
		snprintf(hdr, sizeof(hdr), "АВАРИЯ %u/%u", (unsigned)cur_idx, (unsigned)total);
	}
	uiSetTopHeaderText(hdr);
}

void mainscreenView::uiSetTopHeaderText(const char* text)
{
	if (text == nullptr) {
		return;
	}
	if (std::strncmp(s_top_header_text, text, sizeof(s_top_header_text)) == 0) {
		return;
	}
	std::strncpy(s_top_header_text, text, sizeof(s_top_header_text) - 1u);
	s_top_header_text[sizeof(s_top_header_text) - 1u] = '\0';
	for (uint16_t i = 0; i < TEXTAREATIME_TOP_BAR_SIZE; i++) {
		textAreatime_top_barBuffer[i] = 0;
	}
	Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(s_top_header_text),
			  textAreatime_top_barBuffer, TEXTAREATIME_TOP_BAR_SIZE);
	textAreatime_top_barBuffer[TEXTAREATIME_TOP_BAR_SIZE - 1] = 0;
	textAreatime_top_bar.setWildcard(textAreatime_top_barBuffer);
	textAreatime_top_bar.invalidate();
}

void mainscreenView::updateFireStatus(bool active, uint8_t mode, uint8_t zone, uint8_t remaining_s, uint8_t nZoneNames,
				      char (*zoneNames)[ZONE_NAME_SIZE + 1])
{
	(void)zone;
	uint32_t now = HAL_GetTick();
	fireUiActive = active;
	s_fire_mode = mode;
	if (active) {
		s_warning_manual_select = 0u;
		s_warning_nav_last_press_ms = 0u;
		if (mode == 1u || mode == 5u) {
			ui_set_warning_header_visible(this, true);
			uiSetTopHeaderText((mode == 5u) ? "ПАУЗА" : "ДО ПУСКА");
		} else if (mode == 6u) {
			ui_set_warning_header_visible(this, true);
			uiSetTopHeaderText("");
		} else if (s_banner_mode != BANNER_WARNING) {
			ui_set_warning_header_visible(this, false);
		}
	}

	static uint8_t lastActive = 0xFFu;
	static uint8_t lastMode = 0xFFu;
	static uint8_t lastRemaining = 0xFFu;
	const bool timerDirty = ((uint8_t)active != lastActive || mode != lastMode || remaining_s != lastRemaining);

	if (!active) {
		s_fire_manual_select = 0u;
		s_fire_nav_last_press_ms = 0u;
		Fire_UiSetManualSelection(0u, 0u);
		lastActive = (uint8_t)active;
		lastMode = mode;
		lastRemaining = remaining_s;
		s_fn_ph = PH_IDLE;
		s_fn_n = 0u;
		/* Если сейчас отображается предупреждение, не затираем поля:
		 * warning-логика использует те же widgets и сама их контролирует. */
		if (s_banner_mode != BANNER_WARNING) {
			for (uint16_t i = 0; i < TEXTAREA1_SIZE; i++) {
				textArea1Buffer[i] = 0;
			}
			Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE, "%s", "");
			textArea1.setWildcard(textArea1Buffer);
			textArea1.invalidate();
			CustomContainerSrollText.setText("");
			s_banner_mode = BANNER_NONE;
		}
		/* Не возвращаем время/top bar, если сейчас активны предупреждения. */
		if (s_banner_mode != BANNER_WARNING) {
			ui_set_warning_header_visible(this, false);
			s_top_header_text[0] = '\0';
		}
		return;
	}

	if (nZoneNames > 0u && !fire_list_equals(nZoneNames, zoneNames)) {
		fire_copy_list(nZoneNames, zoneNames);
		s_fn_cur = 0u;
		s_fn_ph = PH_IDLE;
		s_fn_hold_from = 0u;
		if (s_fire_manual_select && s_fn_n > 0u) {
			Fire_UiSetManualSelection(1u, s_fn_cur);
		}
		fireShowCurrentZone();
	}

	if (timerDirty) {
		lastActive = (uint8_t)active;
		lastMode = mode;
		lastRemaining = remaining_s;
		for (uint16_t i = 0; i < TEXTAREA1_SIZE; i++) {
			textArea1Buffer[i] = 0;
		}
		// 10 символов
		char buf[32];
		if (mode == 1u || mode == 5u) {
			snprintf(buf, sizeof(buf), "%uС", (unsigned)remaining_s);
		} else if (mode == 6u) {
			snprintf(buf, sizeof(buf), "ПОЖАР1");
		} else if (mode == 2u) {
			snprintf(buf, sizeof(buf), "ТУШЕНИЕ");
									//  1234567890
		} else if (mode == 3u) {
			snprintf(buf, sizeof(buf), "ТУШ.ВЫП.");
									//  1234567890
		} else if (mode == 4u) {
			snprintf(buf, sizeof(buf), "ПОЖАР/ОСТ.");
									//  1234567890
		} else if (remaining_s > 0u) {
			snprintf(buf, sizeof(buf), "%u", (unsigned)remaining_s);
		} else {
			buf[0] = '\0';
		}
		Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(buf), textArea1Buffer, TEXTAREA1_SIZE);
		textArea1Buffer[TEXTAREA1_SIZE - 1] = 0;
		textArea1.setWildcard(textArea1Buffer);
		textArea1.invalidate();
	}

	if (nZoneNames == 0u) {
		/* Не затираем бегущую строку при active: Model может ещё не получить список зон,
		 * а тики TouchGFX с n==0 иначе держат пустоту до смены секунды таймера. */
		if (!active) {
			CustomContainerSrollText.setText("");
		}
		return;
	}

	if (!s_fire_manual_select &&
	    s_fn_ph == PH_HOLD_3S && s_fn_hold_from != 0u &&
	    (now - s_fn_hold_from) >= FIRE_NAME_HOLD_MS) {
		s_fn_cur = (uint8_t)((s_fn_cur + 1u) % s_fn_n);
		s_fn_ph = PH_IDLE;
		s_fn_hold_from = 0u;
	}

	if (!s_fire_manual_select && s_fn_ph == PH_IDLE) {
		fireShowCurrentZone();
	}
}

void mainscreenView::updateWarningStatus(bool active, uint8_t nItems, char (*bigTitles)[WARNING_TITLE_LEN],
					 char (*details)[ZONE_NAME_SIZE + 1])
{
	if (fireUiActive) {
		s_warning_manual_select = 0u;
		s_warning_nav_last_press_ms = 0u;
		return;
	}
	uint32_t now = HAL_GetTick();

	if (!active || nItems == 0u) {
		s_warning_manual_select = 0u;
		s_warning_nav_last_press_ms = 0u;
		s_wn_n = 0u;
		memset(s_wn_is_attention, 0, sizeof(s_wn_is_attention));
		s_wn_ph = PH_IDLE;
		s_wn_hold_from = 0u;
		if (s_banner_mode == BANNER_WARNING) {
			for (uint16_t i = 0; i < TEXTAREA1_SIZE; i++) {
				textArea1Buffer[i] = 0;
			}
			Unicode::snprintf(textArea1Buffer, TEXTAREA1_SIZE, "%s", "");
			textArea1.setWildcard(textArea1Buffer);
			textArea1.invalidate();
			CustomContainerSrollText.setText("");
			s_banner_mode = BANNER_NONE;
		}
		ui_set_warning_header_visible(this, false);
		s_top_header_text[0] = '\0';
		return;
	}

	if (nItems > 16u) {
		nItems = 16u;
	}

	bool changed = (nItems != s_wn_n);
	if (!changed) {
		for (uint8_t i = 0u; i < nItems && !changed; i++) {
			const char* src_title = bigTitles[i];
			if (((uint8_t)bigTitles[i][0]) == 0x01u) {
				src_title = &bigTitles[i][1];
			}
			if (std::strncmp(s_wn_titles[i], src_title, WARNING_TITLE_LEN) != 0 ||
			    std::strncmp(s_wn_details[i], details[i], ZONE_NAME_SIZE + 1) != 0) {
				changed = true;
			}
		}
	}

	if (changed) {
		/* Пытаемся сохранить текущую позицию ротации, если текущая строка
		 * присутствует и в новом списке (это устраняет визуальное "смаргивание"). */
		uint8_t keep_idx = 0u;
		uint8_t keep_found = 0u;
		if (s_wn_n > 0u && s_wn_cur < s_wn_n) {
			for (uint8_t i = 0u; i < nItems; i++) {
				const char* src_title = bigTitles[i];
				if (((uint8_t)bigTitles[i][0]) == 0x01u) {
					src_title = &bigTitles[i][1];
				}
				if (std::strncmp(s_wn_titles[s_wn_cur], src_title, WARNING_TITLE_LEN) == 0 &&
				    std::strncmp(s_wn_details[s_wn_cur], details[i], ZONE_NAME_SIZE + 1) == 0) {
					keep_idx = i;
					keep_found = 1u;
					break;
				}
			}
		}
		s_wn_n = nItems;
		for (uint8_t i = 0u; i < s_wn_n; i++) {
			const char* src_title = bigTitles[i];
			s_wn_is_attention[i] = 0u;
			if (((uint8_t)bigTitles[i][0]) == 0x01u) {
				s_wn_is_attention[i] = 1u;
				src_title = &bigTitles[i][1];
			}
			std::strncpy(s_wn_titles[i], src_title, WARNING_TITLE_LEN - 1u);
			s_wn_titles[i][WARNING_TITLE_LEN - 1u] = '\0';
			std::strncpy(s_wn_details[i], details[i], ZONE_NAME_SIZE);
			s_wn_details[i][ZONE_NAME_SIZE] = '\0';
		}
		s_wn_cur = keep_found ? keep_idx : 0u;
		s_wn_ph = PH_IDLE;
		s_wn_hold_from = 0u;
		warningShowCurrent();
	}

	if (s_wn_n == 0u) {
		return;
	}

	if (!s_warning_manual_select &&
	    s_wn_ph == PH_HOLD_3S && s_wn_hold_from != 0u &&
	    (now - s_wn_hold_from) >= FIRE_NAME_HOLD_MS) {
		s_wn_cur = (uint8_t)((s_wn_cur + 1u) % s_wn_n);
		s_wn_ph = PH_IDLE;
		s_wn_hold_from = 0u;
		ui_update_warning_header(this);
	}

	if (!s_warning_manual_select && s_wn_ph == PH_IDLE) {
		warningShowCurrent();
	}
}

void mainscreenView::handleMainNavButton(uint8_t but)
{
	if (but == BUT_ESC) {
		if (s_fire_manual_select) {
			s_fire_manual_select = 0u;
			Fire_UiSetManualSelection(0u, 0u);
			s_fn_ph = PH_IDLE;
			s_fn_hold_from = 0u;
			s_fire_nav_last_press_ms = 0u;
			fireShowCurrentZone();
			return;
		}
		if (s_warning_manual_select) {
			s_warning_manual_select = 0u;
			s_wn_ph = PH_IDLE;
			s_wn_hold_from = 0u;
			s_warning_nav_last_press_ms = 0u;
			warningShowCurrent();
			return;
		}
		return;
	}

	if (but != BUT_UP && but != BUT_DOWN) {
		return;
	}

	if (fireUiActive && s_fn_n > 0u) {
		if (!s_fire_manual_select) {
			s_fire_manual_select = 1u;
			s_fn_cur = 0u;
		} else if (but == BUT_UP) {
			s_fn_cur = (uint8_t)((s_fn_cur + 1u) % s_fn_n);
		} else {
			s_fn_cur = (uint8_t)((s_fn_cur == 0u) ? (s_fn_n - 1u) : (s_fn_cur - 1u));
		}
		s_fire_nav_last_press_ms = HAL_GetTick();
		Fire_UiSetManualSelection(1u, s_fn_cur);
		fireShowCurrentZone();
		return;
	}

	if (s_wn_n > 0u) {
		if (!s_warning_manual_select) {
			s_warning_manual_select = 1u;
			s_wn_cur = 0u;
		} else if (but == BUT_UP) {
			s_wn_cur = (uint8_t)((s_wn_cur + 1u) % s_wn_n);
		} else {
			s_wn_cur = (uint8_t)((s_wn_cur == 0u) ? (s_wn_n - 1u) : (s_wn_cur - 1u));
		}
		s_warning_nav_last_press_ms = HAL_GetTick();
		warningShowCurrent();
	}
}
#endif
