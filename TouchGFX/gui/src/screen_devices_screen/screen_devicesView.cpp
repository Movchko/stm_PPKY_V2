#include <gui/screen_devices_screen/screen_devicesView.hpp>
#include <cstring>
#include <cstdio>

#ifndef SIMULATOR
#include "app.hpp"
#include "device_config.h"

extern PPKYCfg PPKYConfig;

namespace {
static bool isMcuType(uint8_t d_type)
{
    return (d_type == DEVICE_MCU_IGN_TYPE ||
            d_type == DEVICE_MCU_TC_TYPE ||
            d_type == DEVICE_MCU_K1 ||
            d_type == DEVICE_MCU_K2 ||
            d_type == DEVICE_MCU_K3 ||
            d_type == DEVICE_MCU_KR);
}

static const char* mcuTypeShortName(uint8_t d_type)
{
    switch (d_type) {
    case DEVICE_MCU_TC_TYPE:  return "ТС";
    case DEVICE_MCU_IGN_TYPE: return "ИГН";
    case DEVICE_MCU_K1:       return "К1";
    case DEVICE_MCU_K2:       return "К2";
    case DEVICE_MCU_K3:       return "К3";
    case DEVICE_MCU_KR:       return "КР";
    default:                  return "?";
    }
}

static void trimZoneName(char* dst, size_t dst_sz, const int8_t* src, size_t src_len)
{
    if (dst == nullptr || dst_sz == 0u || src == nullptr) {
        return;
    }
    size_t n = (src_len < (dst_sz - 1u)) ? src_len : (dst_sz - 1u);
    for (size_t i = 0; i < n; i++) {
        dst[i] = (char)src[i];
    }
    dst[n] = '\0';
    while (n > 0u && (dst[n - 1u] == ' ' || dst[n - 1u] == '\0')) {
        dst[n - 1u] = '\0';
        n--;
    }
}
} // namespace
#endif

screen_devicesView::screen_devicesView()
{

}

void screen_devicesView::setupScreen()
{
    screen_devicesViewBase::setupScreen();
#ifndef SIMULATOR
    refreshDeviceUi();
#endif
}

void screen_devicesView::tearDownScreen()
{
    screen_devicesViewBase::tearDownScreen();
}

#ifndef SIMULATOR
void screen_devicesView::renderSelected()
{
    if (deviceCount == 0u) {
        return;
    }

    uint8_t currSlot = deviceSlots[selectedIndex];
    uint8_t nextSlot = deviceSlots[(uint8_t)((selectedIndex + 1u) % deviceCount)];
    const Device* curr = &PPKYConfig.CfgDevices[currSlot].UId.devId;
    const Device* next = &PPKYConfig.CfgDevices[nextSlot].UId.devId;

    char zoneName[ZONE_NAME_SIZE + 1] = {0};
    uint8_t zone_idx = (curr->zone == 0u) ? 0u : (uint8_t)(curr->zone - 1u);
    if (zone_idx < ZONE_NUMBER) {
        trimZoneName(zoneName, sizeof(zoneName), PPKYConfig.zone_name[zone_idx], ZONE_NAME_SIZE);
    }
    if (zoneName[0] == '\0') {
        (void)std::snprintf(zoneName, sizeof(zoneName), "Зона %u", (unsigned)curr->zone);
    }
    CustomContainerSrollText.setText(zoneName);

    char currLine[32];
    (void)std::snprintf(currLine, sizeof(currLine), "МКУ %s:%u", mcuTypeShortName(curr->d_type), (unsigned)curr->h_adr);
    Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(currLine), textAreaCurrentMCUBuffer, TEXTAREACURRENTMCU_SIZE);
    textAreaCurrentMCUBuffer[TEXTAREACURRENTMCU_SIZE - 1] = 0;
    textAreaCurrentMCU.setWildcard(textAreaCurrentMCUBuffer);
    textAreaCurrentMCU.invalidate();

    char nextLine[24];
    (void)std::snprintf(nextLine, sizeof(nextLine), "МКУ %s:%u", mcuTypeShortName(next->d_type), (unsigned)next->h_adr);
    Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(nextLine), textArea_next_MCUBuffer, TEXTAREA_NEXT_MCU_SIZE);
    textArea_next_MCUBuffer[TEXTAREA_NEXT_MCU_SIZE - 1] = 0;
    textArea_next_MCU.setWildcard(textArea_next_MCUBuffer);
    textArea_next_MCU.invalidate();
}

void screen_devicesView::refreshDeviceUi()
{
    deviceCount = 0u;
    selectedIndex = 0u;
    for (uint8_t i = 0u; i < 32u; i++) {
        const Device* dev = &PPKYConfig.CfgDevices[i].UId.devId;
        if (isMcuType(dev->d_type)) {
            deviceSlots[deviceCount++] = i;
        }
    }

    if (deviceCount == 0u) {
        CustomContainerSrollText.setText("Нет МКУ в конфигурации");
        Unicode::fromUTF8(reinterpret_cast<const uint8_t*>("МКУ: -"), textAreaCurrentMCUBuffer, TEXTAREACURRENTMCU_SIZE);
        textAreaCurrentMCUBuffer[TEXTAREACURRENTMCU_SIZE - 1] = 0;
        textAreaCurrentMCU.setWildcard(textAreaCurrentMCUBuffer);
        textAreaCurrentMCU.invalidate();

        Unicode::fromUTF8(reinterpret_cast<const uint8_t*>("Далее: -"), textArea_next_MCUBuffer, TEXTAREA_NEXT_MCU_SIZE);
        textArea_next_MCUBuffer[TEXTAREA_NEXT_MCU_SIZE - 1] = 0;
        textArea_next_MCU.setWildcard(textArea_next_MCUBuffer);
        textArea_next_MCU.invalidate();
        return;
    }

    renderSelected();
}

void screen_devicesView::nextDevice()
{
    if (deviceCount == 0u) {
        return;
    }
    selectedIndex = (uint8_t)((selectedIndex + 1u) % deviceCount);
    renderSelected();
}

void screen_devicesView::prevDevice()
{
    if (deviceCount == 0u) {
        return;
    }
    selectedIndex = (uint8_t)((selectedIndex == 0u) ? (deviceCount - 1u) : (selectedIndex - 1u));
    renderSelected();
}
#endif
