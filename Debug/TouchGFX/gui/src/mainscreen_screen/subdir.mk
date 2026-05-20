################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.cpp \
../TouchGFX/gui/src/mainscreen_screen/mainscreenView.cpp 

OBJS += \
./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.o \
./TouchGFX/gui/src/mainscreen_screen/mainscreenView.o 

CPP_DEPS += \
./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.d \
./TouchGFX/gui/src/mainscreen_screen/mainscreenView.d 


# Each subdirectory must supply rules for building sources it contributes
TouchGFX/gui/src/mainscreen_screen/%.o TouchGFX/gui/src/mainscreen_screen/%.su TouchGFX/gui/src/mainscreen_screen/%.cyclo: ../TouchGFX/gui/src/mainscreen_screen/%.cpp TouchGFX/gui/src/mainscreen_screen/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-TouchGFX-2f-gui-2f-src-2f-mainscreen_screen

clean-TouchGFX-2f-gui-2f-src-2f-mainscreen_screen:
	-$(RM) ./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.cyclo ./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.d ./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.o ./TouchGFX/gui/src/mainscreen_screen/mainscreenPresenter.su ./TouchGFX/gui/src/mainscreen_screen/mainscreenView.cyclo ./TouchGFX/gui/src/mainscreen_screen/mainscreenView.d ./TouchGFX/gui/src/mainscreen_screen/mainscreenView.o ./TouchGFX/gui/src/mainscreen_screen/mainscreenView.su

.PHONY: clean-TouchGFX-2f-gui-2f-src-2f-mainscreen_screen

