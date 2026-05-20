################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.cpp \
../TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.cpp 

OBJS += \
./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.o \
./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.o 

CPP_DEPS += \
./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.d \
./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.d 


# Each subdirectory must supply rules for building sources it contributes
TouchGFX/gui/src/screenmenu_screen/%.o TouchGFX/gui/src/screenmenu_screen/%.su TouchGFX/gui/src/screenmenu_screen/%.cyclo: ../TouchGFX/gui/src/screenmenu_screen/%.cpp TouchGFX/gui/src/screenmenu_screen/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-TouchGFX-2f-gui-2f-src-2f-screenmenu_screen

clean-TouchGFX-2f-gui-2f-src-2f-screenmenu_screen:
	-$(RM) ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.cyclo ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.d ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.o ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuPresenter.su ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.cyclo ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.d ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.o ./TouchGFX/gui/src/screenmenu_screen/ScreenMenuView.su

.PHONY: clean-TouchGFX-2f-gui-2f-src-2f-screenmenu_screen

