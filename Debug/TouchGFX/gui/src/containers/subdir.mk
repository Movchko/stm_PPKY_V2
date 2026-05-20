################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../TouchGFX/gui/src/containers/CustomContainerScrollTime.cpp \
../TouchGFX/gui/src/containers/CustomContainerSollText.cpp \
../TouchGFX/gui/src/containers/CustomContainerTime.cpp \
../TouchGFX/gui/src/containers/CustomContainerTopBar.cpp \
../TouchGFX/gui/src/containers/CustomContainerTopBar_1.cpp \
../TouchGFX/gui/src/containers/mainmenu.cpp 

OBJS += \
./TouchGFX/gui/src/containers/CustomContainerScrollTime.o \
./TouchGFX/gui/src/containers/CustomContainerSollText.o \
./TouchGFX/gui/src/containers/CustomContainerTime.o \
./TouchGFX/gui/src/containers/CustomContainerTopBar.o \
./TouchGFX/gui/src/containers/CustomContainerTopBar_1.o \
./TouchGFX/gui/src/containers/mainmenu.o 

CPP_DEPS += \
./TouchGFX/gui/src/containers/CustomContainerScrollTime.d \
./TouchGFX/gui/src/containers/CustomContainerSollText.d \
./TouchGFX/gui/src/containers/CustomContainerTime.d \
./TouchGFX/gui/src/containers/CustomContainerTopBar.d \
./TouchGFX/gui/src/containers/CustomContainerTopBar_1.d \
./TouchGFX/gui/src/containers/mainmenu.d 


# Each subdirectory must supply rules for building sources it contributes
TouchGFX/gui/src/containers/%.o TouchGFX/gui/src/containers/%.su TouchGFX/gui/src/containers/%.cyclo: ../TouchGFX/gui/src/containers/%.cpp TouchGFX/gui/src/containers/subdir.mk
	arm-none-eabi-g++ "$<" -mcpu=cortex-m33 -std=gnu++14 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H523xx -c -I../Core/Inc -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib/include" -I../Drivers/STM32H5xx_HAL_Driver/Inc -I../Drivers/STM32H5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H5xx/Include -I../Drivers/CMSIS/Include -I../TouchGFX/App -I../TouchGFX/target/generated -I../TouchGFX/target -I../Middlewares/ST/touchgfx/framework/include -I../TouchGFX/generated/fonts/include -I../TouchGFX/generated/gui_generated/include -I../TouchGFX/generated/images/include -I../TouchGFX/generated/texts/include -I../TouchGFX/generated/videos/include -I../TouchGFX/gui/include -I"C:/Users/79099/STM32CubeIDE/workspace_1.13.2/device_lib" -O0 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-TouchGFX-2f-gui-2f-src-2f-containers

clean-TouchGFX-2f-gui-2f-src-2f-containers:
	-$(RM) ./TouchGFX/gui/src/containers/CustomContainerScrollTime.cyclo ./TouchGFX/gui/src/containers/CustomContainerScrollTime.d ./TouchGFX/gui/src/containers/CustomContainerScrollTime.o ./TouchGFX/gui/src/containers/CustomContainerScrollTime.su ./TouchGFX/gui/src/containers/CustomContainerSollText.cyclo ./TouchGFX/gui/src/containers/CustomContainerSollText.d ./TouchGFX/gui/src/containers/CustomContainerSollText.o ./TouchGFX/gui/src/containers/CustomContainerSollText.su ./TouchGFX/gui/src/containers/CustomContainerTime.cyclo ./TouchGFX/gui/src/containers/CustomContainerTime.d ./TouchGFX/gui/src/containers/CustomContainerTime.o ./TouchGFX/gui/src/containers/CustomContainerTime.su ./TouchGFX/gui/src/containers/CustomContainerTopBar.cyclo ./TouchGFX/gui/src/containers/CustomContainerTopBar.d ./TouchGFX/gui/src/containers/CustomContainerTopBar.o ./TouchGFX/gui/src/containers/CustomContainerTopBar.su ./TouchGFX/gui/src/containers/CustomContainerTopBar_1.cyclo ./TouchGFX/gui/src/containers/CustomContainerTopBar_1.d ./TouchGFX/gui/src/containers/CustomContainerTopBar_1.o ./TouchGFX/gui/src/containers/CustomContainerTopBar_1.su ./TouchGFX/gui/src/containers/mainmenu.cyclo ./TouchGFX/gui/src/containers/mainmenu.d ./TouchGFX/gui/src/containers/mainmenu.o ./TouchGFX/gui/src/containers/mainmenu.su

.PHONY: clean-TouchGFX-2f-gui-2f-src-2f-containers

